namespace Foundation::RenderCore
{
    uint32_t BindlessPool::Update(uint32_t id, RHITextureView* view)
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
    uint32_t BindlessPool::Allocate(RHITextureView* view)
    {
        Binding* binding = mBindings.Construct(Binding{0uLL, {view->GetTexture()}, {view}});
        return Update(binding->id = mBindings.Index(binding), view);
    }
    uint32_t BindlessPool::Allocate(RHIDeviceScopedObjectHandle<RHITexture>&& texture, RHITextureView* view)
    {
        Binding* binding = mBindings.Construct(Binding{0uLL, {std::move(texture)}, {view}});
        return Update(binding->id = mBindings.Index(binding), view);
    }
    void BindlessPool::Free(uint32_t id)
    {
        Binding* ptr = mBindings.At(id);
        if (ptr)
            mBindings.Destruct(ptr);
    }
} // namespace Foundation::RenderCore
