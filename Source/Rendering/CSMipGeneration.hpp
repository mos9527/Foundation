#pragma once
#include <RenderCore/Renderer.hpp>
namespace Foundation::Rendering
{
    using namespace RenderCore;
    inline void createCSMipGenerationPasses(Renderer* renderer, StringView name, ResourceHandle src, ResourceHandle dst,
                                            RHITextureAspectFlagBits srcAspect, RHIResourceFormat srcFormat,
                                            RHITextureAspectFlagBits dstAspect, RHIResourceFormat dstFormat,
                                            uint32_t maxMips = 16, uint32_t layer = 0
                                            )
    {
        using namespace Math;
#pragma pack(push, 1)
        struct PushConstant
        {
            ivec2 srcExtent;
            uint filter;
        };
#pragma pack(pop)
        for (uint32 i = 0; i < maxMips; ++i)
        {
            createPass(
                renderer, fmt::format("Mip Gen {} {}", i, name), RHIDeviceQueueType::Compute,
                [=](PassHandle self, Renderer* r)
                {
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
                    r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PushConstant));
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    RHITexture* srcTex = r->DerefResource(src).Get<RHITexture*>();
                    RHITexture* dstTex = r->DerefResource(dst).Get<RHITexture*>();
                    RHIExtent3D extent;
                    CHECK_MSG((extent = srcTex->mDesc.extent) == dstTex->mDesc.extent, "Extent of source images mismatch");
                    r->CmdSetPipeline(self,cmd);
                    uint32_t w = std::max(1u, extent.x >> i);
                    uint32_t h = std::max(1u, extent.y >> i);
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, PushConstant{
                        .srcExtent = { w, h },
                        .filter = 0 // box
                    });
                    r->CmdDispatch(self, cmd, {w,h,1});
                },
                [=](PassHandle self, Renderer* r)
                {
                    if (i == 0 && src == dst)
                        return true;
                    RHITexture* srcTex = r->DerefResource(src).Get<RHITexture*>();
                    return i > srcTex->mDesc.mipLevels - 1;
                });
        }
    }
} // namespace Foundation::Rendering
