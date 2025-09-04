#include "Renderer.hpp"
#include "GBuffer.hpp"
using namespace Foundation;
GBuffer::GBuffer(Renderer& renderer, ResourceHandle sceneGlobal, ResourceHandle sceneInstance, ResourceHandle scenePrimitive) :
    sceneGlobal(sceneGlobal), sceneInstance(sceneInstance), scenePrimitive(scenePrimitive) {
    RHIExtent3D extents = { renderer.GetSwapchainExtent().x, renderer.GetSwapchainExtent().y, 1 };
    m_albedo = renderer.CreateResource("GBuffer.Albedo", RHITextureDesc{
        .usage = RHITextureUsageBits::RenderTarget | RHITextureUsageBits::SampledImage,
        .extent = extents,
        .format = RHIResourceFormat::R8G8B8A8_UNORM,
        .initial_layout = RHITextureLayout::RenderTarget
    });
    m_depth = renderer.CreateResource("GBuffer.Depth", RHITextureDesc{
        .usage = RHITextureUsageBits::DepthStencil,
        .extent = extents,
        .format = RHIResourceFormat::D32_SIGNED_FLOAT,
        .initial_layout = RHITextureLayout::DepthStencil
    });
}
void GBuffer::Setup(PassHandle self, Renderer& renderer) {
    m_albedoView = renderer.BindTextureRTV(self, m_albedo, { .format = RHIResourceFormat::R8G8B8A8_UNORM });
    m_depthView  = renderer.BindTextureDSV(self, m_depth,  { .format = RHIResourceFormat::D32_SIGNED_FLOAT });
}
void GBuffer::Record(PassHandle self, Renderer&, RHICommandList* cmd) {

}
