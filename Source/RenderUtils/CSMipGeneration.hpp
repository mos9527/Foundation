#pragma once
#include <RenderCore/Renderer.hpp>
namespace Foundation::RenderUtils
{
    using namespace RenderCore;
    inline void createCSMipGenerationPasses(Renderer* renderer, StringView name, RHIDeviceQueueType queue, ResourceHandle src, ResourceHandle dst,
                                            RHIExtent2D srcExtent,
                                            RHITextureAspectFlagBits srcAspect, RHIResourceFormat srcFormat,
                                            RHITextureAspectFlagBits dstAspect, RHIResourceFormat dstFormat,
                                            uint32_t maxMips = 16, uint32_t layer = 0, RHIDeviceSampler::SamplerDesc samplerDesc = {}
                                            )
    {
        using namespace Math;
        for (uint32 i = 0; i < maxMips; ++i)
        {
            renderer->CreatePass(
                fmt::format("Mip Gen {} {}", i, name), queue, 0u,
                [=](PassHandle self, Renderer* r)
                {
                    auto sampler = renderer->CreateSampler(samplerDesc);
                    r->BindTextureSampler(self, sampler, "sampler");
                    r->BindShader(self, RHIShaderStageBits::Compute, "csMain", "data/shaders/CSMipGeneration.spv");
                    if (i == 0)
                        r->BindTextureSRV(
                                self, src, "srcTexture", RHIPipelineStageBits::ComputeShader,
                                {
                                    .format = srcFormat,
                                    .range = RHITextureSubresourceRange::Create(srcAspect, 0, 1, layer, 1)
                        });
                    else
                        r->BindTextureSRV(
                            self, dst, "srcTexture", RHIPipelineStageBits::ComputeShader,
                            {
                                .format = dstFormat,
                                .range = RHITextureSubresourceRange::Create(dstAspect, i - 1, 1, layer, 1)
                            });
                    r->BindTextureUAV(
                        self, dst, "dstTexture", RHIPipelineStageBits::ComputeShader,
                        {
                            .format = dstFormat,
                            .range = RHITextureSubresourceRange::Create(dstAspect, i, 1, layer, 1)
                        });
                    r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(float2));
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    RHITexture* dstTex = r->DerefResource(dst).Get<RHITexture*>();
                    RHIExtent3D extent = dstTex->mDesc.extent;
                    r->CmdSetPipeline(self,cmd);
                    uint32_t w = std::max(1u, extent.x >> i);
                    uint32_t h = std::max(1u, extent.y >> i);
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, float2{w,h});
                    r->CmdDispatch(self, cmd, {w,h,1});
                },
                [=](PassHandle, Renderer* r)
                {
                    RHITexture* dstTex = r->DerefResource(dst).Get<RHITexture*>();
                    return (src == dst && i == 0) || i >= dstTex->mDesc.mipLevels;
                });
        }
    }
} // namespace Foundation::Rendering
