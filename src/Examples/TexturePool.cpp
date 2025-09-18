#include <Rendering/TexturePool.hpp>
#include "Examples.hpp"
namespace Examples
{
    /**
     * @example TexturePool.cpp
     * Bindless texture pool example
     * @example Shaders/TexturePool.slang
     */
    class TexturePoolApp : public RenderApplication
    {
        UniquePtr<TexturePool> m_textures;
        void OnDeviceSetup() override
        {
            m_textures = ConstructUnique<TexturePool>(GetAllocator(), m_device.Get(), GetAllocator());
        }
        void OnRendererSetup() override
        {
            ResourceHandle sampler = createSampler(m_renderer.get(), {
                .filter = {
                    .min_filter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor,
                    .mag_filter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor
                }
            });
            createPSFullscreenPass(
                m_renderer.get(), "Texture Pool Atlas",
                [=, this](PassHandle self, Renderer* r)
                {
                    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/TexturePool.spv");
                    r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
                    r->BindTextureSampler(self, sampler, "sampler");
                    r->BindDescriptorSet(self, "textures" , m_textures->GetDescriptorSet(), m_textures->GetDescriptorSetLayout());
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, GetApplicationTime());
                });
        }
    };

} // namespace Examples
int main(int argc, char** argv)
{
    Examples::TexturePoolApp app;
    app.Initialize<VulkanApplication>({.windowTitle = "Texture Pool"});
    app.RunForever();
}
