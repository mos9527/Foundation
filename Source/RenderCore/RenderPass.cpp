#include "RenderPass.hpp"
namespace Foundation::RenderCore
{
    TrackedPass::TrackedPass(Allocator* alloc, const PassHandle handle, StringView name, RHIDeviceQueueType queue,
                             UniquePtr<RenderPass> renderPass, size_t priority) :
        name(name), handle(handle), priority(priority), queue(queue), textureUsages(alloc), bufferUsages(alloc),
        resources(alloc), texviews(alloc), shaders(alloc), tex_bindings(alloc), buf_bindings(alloc), external_sets(alloc),
        samplers(alloc), push_constants(alloc), rtvs(alloc), vertex_input_bindings(alloc), vertex_input_attributes(alloc),
        pass(std::move(renderPass)), desc_layouts(alloc), p_desc_layouts(alloc), desc_sets(alloc), p_desc_sets(alloc),
        external_desc_sets(alloc) {};
}