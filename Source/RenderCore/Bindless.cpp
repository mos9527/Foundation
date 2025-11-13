namespace Foundation::RenderCore
{
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
        binding->id = mBindings.Index(binding);

        std::unique_lock lock(mDescMutex);
        mDescSet->Update({.binding = 0,
                          .startIndex = binding->id,
                          .type = RHIDescriptorType::SampledImage,
                          .images = {{{.imageView = view, .layout = RHITextureLayout::ShaderReadOnly}}}});
        return binding->id;
    }
    void BindlessPool::Free(uint32_t id)
    {
        Binding* ptr = mBindings.At(id);
        if (ptr)
            mBindings.Destruct(ptr);
    }
} // namespace Foundation::RenderCore
