#pragma once
#include <RenderCore/Renderer.hpp>
#include "RHICore/Device.hpp"
namespace Foundation::RenderUtils
{
    using namespace RenderCore;
    inline void createCSMipGenerationPasses(
        Renderer* renderer, StringView name, RHIDeviceQueueType queue, ResourceHandle src, ResourceHandle dst,
        RHIResourceFormat srcFormat, RHIResourceFormat dstFormat, RHITextureAspectFlag srcAspect,
        RHITextureAspectFlag dstAspect, ResourceHandle srcSampler, uint numMips, uint numLayer = 1,
        RHIDeviceSampler::SamplerDesc::Reduction reduction = RHIDeviceSampler::SamplerDesc::Reduction::WeightedAverage)
    {
        using namespace Math;
        struct PushConstants
        {
            uint2 srcExtents;
            uint mips;
            uint numWorkGroups;
        };
        // From ffx_spd.h
        auto SpdSetup = [](uint2& dispatchThreadGroupCountXY, // CPU side: dispatch thread group count xy
                           uint numWorkGroups, // GPU side: pass in as constant
                           uint2 extent, // width, height
                           uint mips // number of total mip levels
                        )
        {
            uint endIndexX = (extent[0] - 1) / 64; // rectInfo[0] = left, rectInfo[2] = width
            uint endIndexY = (extent[1] - 1) / 64; // rectInfo[1] = top, rectInfo[3] = height

            dispatchThreadGroupCountXY[0] = endIndexX + 1;
            dispatchThreadGroupCountXY[1] = endIndexY + 1;

            numWorkGroups = (dispatchThreadGroupCountXY[0]) * (dispatchThreadGroupCountXY[1]);
        };
        renderer->CreatePass(
            name, queue, 0u,
            [=](PassHandle self, Renderer* r)
            {
                int reductionMode = 0;
                switch (reduction)
                {
                case RHIDeviceSampler::SamplerDesc::Reduction::WeightedAverage:
                    reductionMode = 0;
                    break;
                case RHIDeviceSampler::SamplerDesc::Reduction::Min:
                    reductionMode = 1;
                    break;
                case RHIDeviceSampler::SamplerDesc::Reduction::Max:
                    reductionMode = 2;
                    break;
                }
                r->BindShader(self, RHIShaderStageBits::Compute, "csMain", "data/shaders/CSMipGeneration.spv",
                              AsBytes(AsSpan(reductionMode)));
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PushConstants));
                r->BindTextureSampler(self, srcSampler, "srcSampler");
                CHECK_MSG(numMips <= 12, "CS Mip generation supports up to 12 mips.");
                if (src != dst)
                    r->BindTextureSRV(self, src, "imgSrc", RHIPipelineStageBits::ComputeShader,
                                      {
                                          .format = srcFormat,
                                          .dimension = RHITextureDimension::E2D,
                                          .range = RHITextureSubresourceRange::Create(srcAspect, 0, 1, 0, numLayer),
                                      });
                else
                    r->BindTextureUAV(self, dst, "imgSrc", RHIPipelineStageBits::ComputeShader,
                                      {
                                          .format = srcFormat,
                                          .dimension = RHITextureDimension::E2D,
                                          .range = RHITextureSubresourceRange::Create(srcAspect, 0, 1, 0, numLayer),
                                      });
                for (uint mip = 0; mip < 12; mip++)
                {
                    r->BindTextureUAV(self, dst, fmt::format("imgDst{}", mip), RHIPipelineStageBits::ComputeShader,
                                      {
                                          .format = dstFormat,
                                          .dimension = RHITextureDimension::E2D,
                                          // Later mip levels won't be used by shader if numMips is smaller
                                          // Binding is still required, so repeat the last one.
                                          .range = RHITextureSubresourceRange::Create(
                                              dstAspect, std::min(mip, numMips - 1u), 1, 0, numLayer),
                                      });
                }
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                auto* srcTex = r->DerefResource(src).Get<RHITexture*>();
                PushConstants pc{.srcExtents = {srcTex->mDesc.extent.x, srcTex->mDesc.extent.y}, .mips = numMips};
                uint2 dispatchThreadGroupCountXY;
                SpdSetup(dispatchThreadGroupCountXY, pc.numWorkGroups, pc.srcExtents, numMips);
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, pc);
                cmd->Dispatch(dispatchThreadGroupCountXY.x, dispatchThreadGroupCountXY.y, 1);
            });
    }
} // namespace Foundation::RenderUtils
