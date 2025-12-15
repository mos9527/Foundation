#pragma once
#include <RenderCore/Renderer.hpp>
#include "RHICore/Device.hpp"
namespace Foundation::RenderUtils
{
    using namespace RenderCore;
    inline void createCSMipGenerationSinglePass(
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
            uint sameSrcDst;
        };
        // From ffx_spd.h
        auto SpdSetup = [](uint2& dispatchThreadGroupCountXY, // CPU side: dispatch thread group count xy
                           uint& numWorkGroups, // GPU side: pass in as constant
                           uint2 extent // width, height
                        )
        {
            uint endIndexX = (extent[0] - 1) / 64; // rectInfo[0] = left, rectInfo[2] = width
            uint endIndexY = (extent[1] - 1) / 64; // rectInfo[1] = top, rectInfo[3] = height

            dispatchThreadGroupCountXY[0] = endIndexX + 1;
            dispatchThreadGroupCountXY[1] = endIndexY + 1;

            numWorkGroups = (dispatchThreadGroupCountXY[0]) * (dispatchThreadGroupCountXY[1]);
        };
        auto SpdCounter = renderer->CreateResource(fmt::format("{} SPD Atomics", name),
                                                   RHIBufferDesc{
                                                       .usage = RHIBufferUsageBits::StorageBuffer,
                                                       .size = sizeof(uint) * 6,
                                                   });
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
                r->BindShader(self, RHIShaderStageBits::Compute, "csMain", "data/shaders/CSMipGenerationSinglePass.spv",
                              AsBytes(AsSpan(reductionMode)));
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PushConstants));
                r->BindTextureSampler(self, srcSampler, "srcSampler");
                r->BindBufferUnordered(self, SpdCounter, RHIPipelineStageBits::ComputeShader, "spdGlobalAtomic");
                CHECK_MSG(numMips <= 12, "Single Pass CS Mip generation supports up to 12 mips.");
                CHECK_MSG(numMips > 1, "Single Pass CS Mip generation requires at least 2 mips.");
                r->BindTextureSRV(self, src, "imgSrc", RHIPipelineStageBits::ComputeShader,
                                  {
                                      .format = srcFormat,
                                      .dimension = RHITextureDimension::E2DArray,
                                      .range = RHITextureSubresourceRange::Create(srcAspect, 0, 1, 0, numLayer),
                                  });
                for (uint mip = 0; mip <= 12; mip++)
                {
                    r->BindTextureUAV(self, dst, "imgDst", RHIPipelineStageBits::ComputeShader,
                                      {
                                          .format = dstFormat,
                                          .dimension = RHITextureDimension::E2DArray,
                                          // Later mip levels won't be used by shader if numMips is smaller
                                          // Binding is still required, so repeat the last one.
                                          // Furthermore, for same src/dst, the MIP 0 is not written to.
                                          .range = RHITextureSubresourceRange::Create(
                                              dstAspect, std::min(dst != src ? mip : (std::max(1u, mip)), numMips - 1u),
                                              1, 0, numLayer),
                                      });
                    if (mip == 6)
                        r->BindTextureUAV(self, dst, "imgDst6", RHIPipelineStageBits::ComputeShader,
                                          {
                                              .format = dstFormat,
                                              .dimension = RHITextureDimension::E2DArray,
                                              .range = RHITextureSubresourceRange::Create(
                                                  dstAspect, std::min(mip, numMips - 1u), 1, 0, numLayer),
                                          });
                }
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                auto* dstTex = r->DerefResource(dst).Get<RHITexture*>();
                PushConstants pc{.srcExtents = {dstTex->mDesc.extent.x, dstTex->mDesc.extent.y},
                                 .mips = numMips,
                                 .sameSrcDst = src == dst};
                uint2 dispatchThreadGroupCountXY;
                SpdSetup(dispatchThreadGroupCountXY, pc.numWorkGroups, pc.srcExtents);
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, pc);
                cmd->Dispatch(dispatchThreadGroupCountXY.x, dispatchThreadGroupCountXY.y, 1);
            });
    }

    inline void createCSMipGenerationPasses(Renderer* renderer, StringView name, RHIDeviceQueueType queue,
                                            ResourceHandle src, ResourceHandle dst, RHIResourceFormat srcFormat,
                                            RHIResourceFormat dstFormat, RHITextureAspectFlagBits srcAspect,
                                            RHITextureAspectFlagBits dstAspect, ResourceHandle srcSampler,
                                            uint32_t numMips, uint32_t layer = 0)
    {
        using namespace Math;
        for (uint32 i = 0; i < numMips; ++i)
        {
            if (i == 0 && src == dst)
                continue;
            renderer->CreatePass(
                fmt::format("Mip Gen {} {}", i, name), queue, 0u,
                [=](PassHandle self, Renderer* r)
                {
                    r->BindTextureSampler(self, srcSampler, "sampler");
                    r->BindShader(self, RHIShaderStageBits::Compute, "csMain", "data/shaders/CSMipGeneration.spv");
                    if (i == 0)
                        r->BindTextureSRV(self, src, "srcTexture", RHIPipelineStageBits::ComputeShader,
                                          {.format = srcFormat,
                                           .range = RHITextureSubresourceRange::Create(srcAspect, 0, 1, layer, 1)});
                    else
                        r->BindTextureSRV(self, dst, "srcTexture", RHIPipelineStageBits::ComputeShader,
                                          {.format = dstFormat,
                                           .range = RHITextureSubresourceRange::Create(dstAspect, i - 1, 1, layer, 1)});
                    r->BindTextureUAV(
                        self, dst, "dstTexture", RHIPipelineStageBits::ComputeShader,
                        {.format = dstFormat, .range = RHITextureSubresourceRange::Create(dstAspect, i, 1, layer, 1)});
                    r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(float2));
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    RHITexture* dstTex = r->DerefResource(dst).Get<RHITexture*>();
                    RHIExtent3D extent = dstTex->mDesc.extent;
                    r->CmdSetPipeline(self, cmd);
                    uint32_t w = std::max(1u, extent.x >> i);
                    uint32_t h = std::max(1u, extent.y >> i);
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, float2{w, h});
                    r->CmdDispatch(self, cmd, {w, h, 1});
                });
        }
    }
} // namespace Foundation::RenderUtils
