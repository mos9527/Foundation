#pragma once
#include <RenderCore/Renderer.hpp>
namespace Foundation::Rendering
{
    using namespace RenderCore;
    inline void createCSMipGenerationPasses(Renderer* renderer, StringView name, ResourceHandle src, ResourceHandle dst,
                                            RHIExtent3D extent, RHITextureAspectFlagBits aspect, RHIResourceFormat format,
                                            uint32_t layer = 0
                                            )
    {
        using namespace Math;
        CHECK_MSG((extent.x & (extent.x - 1)) == 0 && (extent.y & (extent.y - 1)) == 0, "Extent must be power of two");
        uint32_t dim = std::min(extent.x, extent.y);
        uint32_t mips = 32u - std::countl_zero(dim);
        CHECK_MSG(mips, "Extent must be at least 1x1");
        // Mip 0 - Copy or ignore if src == dst
        if (src != dst)
        {
            createPass(
                renderer, fmt::format("Mip Copy {}", name), RHIDeviceQueueType::Compute,
                [=](PassHandle self, Renderer* r)
                {
                    r->BindBufferCopySrc(self, src);
                    r->BindBufferCopyDst(self, dst);
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    RHITexture* srcTex = r->DerefResource(src).Get<RHITexture*>();
                    RHITexture* dstTex = r->DerefResource(dst).Get<RHITexture*>();
                    cmd->CopyImage(
                        srcTex, RHITextureLayout::TransferSrc,
                        dstTex, RHITextureLayout::TransferDst,
                                   {{
                                       {
                                           .srcLayer = {
                                               .aspect = aspect,
                                               .mipLevel = 0,
                                               .baseArrayLayer = layer,
                                               .layerCount = 1,
                                           },
                                           .dstLayer = {
                                                .aspect = aspect,
                                                .mipLevel = 0,
                                                .baseArrayLayer = layer,
                                                .layerCount = 1,
                                           },
                                           .extent = extent
                                       }}});
                });
        }
#pragma pack(push, 1)
        struct PushConstant
        {
            ivec2 srcExtent;
            uint filter;
        };
#pragma pack(pop)
        for (uint32 i = 1; i <= mips; ++i)
        {
            createPass(
                renderer, fmt::format("Mip Gen {} {}", i, name), RHIDeviceQueueType::Compute,
                [=](PassHandle self, Renderer* r)
                {
                    r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/CSMipGeneration.spv");
                    r->BindTextureSRV(
                        self, dst, "srcTexture", RHIPipelineStageBits::ComputeShader,
                        {
                            .format = format,
                            .range = RHITextureSubresourceRange::Create(aspect, i - 1, 1, layer, 1)
                        });
                    r->BindTextureUAV(
                        self, dst, "dstTexture", RHIPipelineStageBits::ComputeShader,
                        {
                            .format = format,
                            .range = RHITextureSubresourceRange::Create(aspect, i, 1, layer, 1)
                        });
                    r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PushConstant));
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    r->CmdSetPipeline(self,cmd);
                    uint32_t w = std::max(1u, extent.x >> i);
                    uint32_t h = std::max(1u, extent.y >> i);
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, PushConstant{
                        .srcExtent = { std::max(1u, extent.x >> (i - 1)), std::max(1u, extent.y >> (i - 1)) },
                        .filter = 0 // box
                    });
                    r->CmdDispatch(self, cmd, {w,h,1});
                });
        }
    }
} // namespace Foundation::Rendering
