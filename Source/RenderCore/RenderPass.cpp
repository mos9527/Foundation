namespace Foundation::RenderCore
{
    TrackedPass::TrackedPass(Allocator* alloc, const PassHandle handle, StringView name, RHIDeviceQueueType queue,
                             UniquePtr<RenderPass> renderPass, size_t priority) :
        name(name), handle(handle), priority(priority), queue(queue), textureUsages(alloc), bufferUsages(alloc),
        resources(alloc), texviews(alloc), shaders(alloc), explictDescriptorBindings(alloc), textureBindings(alloc), bufferBindings(alloc),
        externalBindings(alloc), samplers(alloc), pushConstants(alloc), rtvs(alloc), vertexInputBindings(alloc),
        vertexInputAttributes(alloc), pass(std::move(renderPass)), descriptorLayouts(alloc), pDescriptorLayouts(alloc), descriptorSets(alloc),
        pDescriptorSets(alloc), pExternalDescriptorSets(alloc) {};
}