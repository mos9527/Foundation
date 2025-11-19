void RendererSetup(FEditorContext* context, RendererDesc const& desc)
{
    context->renderer.reset();
    context->renderer = ConstructUnique<Renderer>(context->allocator, desc, context->device,
                                                  context->swapchain, context->allocator);
}
