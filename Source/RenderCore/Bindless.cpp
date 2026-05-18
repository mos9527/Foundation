namespace Foundation::RenderCore
{
    uint32_t BindlessPool::UpdateDescriptor(uint32_t id, RHITextureView* view)
    {
        std::unique_lock lock(mDescMutex);
        mDescSet->Update({.binding = 0,
                          .startIndex = id,
                          .type = RHIDescriptorType::SampledImage,
                          .images = {{{.imageView = view, .layout = RHITextureLayout::ShaderReadOnly}}}});
        return id;
    }
    BindlessPool::BindlessPool(RHIDevice* device, Allocator* allocator, BindlessPoolDesc const& desc) :
        mDevice(device), mAllocator(allocator), mBindings(desc.maxBindings, allocator), mIdleGuard(device), mDesc(desc)
    {
        mDescPool = mDevice->CreateDescriptorPool(
            {.bindings = {{{.type = RHIDescriptorType::SampledImage, .maxCount = desc.maxBindings}}},
             .updateAfterBind = true});
        mDescLayout = mDevice->CreateDescriptorSetLayout({.bindings = {{{.count = desc.maxBindings,
                                                                         .stage = RHIShaderStageBits::All,
                                                                         .type = RHIDescriptorType::SampledImage}}},
                                                          .updateAfterBind = true});
        mDescSet = mDescPool->CreateDescriptorSet(mDescLayout, desc.maxBindings);
    }
    void BindlessPool::AddStats(Binding const& binding)
    {
        const size_t bytes = binding.resource.Visit(
            [](RHITexture* texture) -> size_t { return texture ? texture->GetAllocationSize() : 0; },
            [](RHIDeviceScopedHandle<RHITexture> const& texture) -> size_t
            {
                RHITexture* ptr = texture.Get();
                return ptr ? ptr->GetAllocationSize() : 0;
            });
        mActiveBindings++;
        mReferencedTextureBytes += bytes;
        if (binding.resource.GetIf<RHIDeviceScopedHandle<RHITexture>>())
        {
            mOwnedTextureBindings++;
            mOwnedTextureBytes += bytes;
        }
    }
    void BindlessPool::RemoveStats(Binding const& binding)
    {
        const size_t bytes = binding.resource.Visit(
            [](RHITexture* texture) -> size_t { return texture ? texture->GetAllocationSize() : 0; },
            [](RHIDeviceScopedHandle<RHITexture> const& texture) -> size_t
            {
                RHITexture* ptr = texture.Get();
                return ptr ? ptr->GetAllocationSize() : 0;
            });
        mActiveBindings--;
        mReferencedTextureBytes -= bytes;
        if (binding.resource.GetIf<RHIDeviceScopedHandle<RHITexture>>())
        {
            mOwnedTextureBindings--;
            mOwnedTextureBytes -= bytes;
        }
    }
    uint32_t BindlessPool::Allocate(RHITextureView* view)
    {
        Binding* binding = mBindings.Construct(Binding{0uLL, {view->GetTexture()}, {view}});
        binding->id = mBindings.Index(binding);
        AddStats(*binding);
        return UpdateDescriptor(binding->id, view);
    }
    uint32_t BindlessPool::Allocate(RHIDeviceScopedHandle<RHITexture>&& texture, RHITextureScopedHandle<RHITextureView>&& view)
    {
        RHITextureView* rawView = view.Get();
        Binding* binding = mBindings.Construct(Binding{0uLL, {std::move(texture)}, {std::move(view)}});
        binding->id = mBindings.Index(binding);
        AddStats(*binding);
        return UpdateDescriptor(binding->id, rawView);
    }
    uint32_t BindlessPool::Update(uint32_t id, RHIDeviceScopedHandle<RHITexture>&& texture, RHITextureScopedHandle<RHITextureView>&& view)
    {
        Binding* binding = mBindings.At(id);
        CHECK_MSG(binding, "Cannot update invalid bindless texture binding {}", id);
        RHITextureView* rawView = view.Get();
        UpdateDescriptor(id, rawView);
        mIdleGuard.WaitIdle();
        RemoveStats(*binding);
        std::destroy_at(binding);
        std::construct_at(binding, Binding{0uLL, {std::move(texture)}, {std::move(view)}});
        binding->id = id;
        AddStats(*binding);
        return id;
    }
    void BindlessPool::Free(uint32_t id)
    {
        Binding* ptr = mBindings.At(id);
        if (ptr)
        {
            RemoveStats(*ptr);
            mBindings.Destruct(ptr);
        }
    }
    RHITexture* BindlessPool::GetResource(uint32_t id)
    {
        Binding* binding = mBindings.At(id);
        if (!binding)
            return nullptr;

        return binding->resource.Visit(
            [](RHITexture* texture) -> RHITexture* { return texture; },
            [](RHIDeviceScopedHandle<RHITexture> const& texture) -> RHITexture* { return texture.Get(); });
    }
    RHITextureView* BindlessPool::GetView(uint32_t id)
    {
        Binding* binding = mBindings.At(id);
        if (!binding)
            return nullptr;

        return binding->view.Visit(
            [](RHITextureView* view) -> RHITextureView* { return view; },
            [](RHITextureScopedHandle<RHITextureView> const& view) -> RHITextureView* { return view.Get(); });
    }
    BindlessPool::Stats BindlessPool::GetStats() const
    {
        return {
            .activeBindings = mActiveBindings,
            .capacity = mDesc.maxBindings,
            .ownedTextureBindings = mOwnedTextureBindings,
            .referencedTextureBytes = mReferencedTextureBytes,
            .ownedTextureBytes = mOwnedTextureBytes,
        };
    }
} // namespace Foundation::RenderCore
