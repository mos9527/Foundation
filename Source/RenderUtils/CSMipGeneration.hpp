#pragma once
#include <Core/Paths.hpp>
#include <RenderCore/Renderer.hpp>
#include <RHICore/Device.hpp>
#include <Math/Math.hpp>
namespace Foundation::RenderUtils
{
    using namespace RenderCore;
    /**
     * @brief Generates full mip-chain with multiple compute dispatches.
     * @note When src != dst, the first *generated* mip (i.e. mip 1 of the src) is written to the dst's mip *0*.
     */
    inline void createCSMipGenerationPasses(Renderer* renderer, StringView name, RHIDeviceQueueType queue,
                                            ResourceHandle src, ResourceHandle dst, RHIResourceFormat srcFormat,
                                            RHIResourceFormat dstFormat, RHITextureAspectFlagBits srcAspect,
                                            RHITextureAspectFlagBits dstAspect, ResourceHandle srcSampler,
                                            uint32_t numMips, uint32_t layer = 0)
    {
        using namespace Math;
        for (uint32 i = 1; i < numMips; ++i)
        {
            renderer->CreatePass(
                fmt::format("Mip Gen {} {}", i, name), queue, 0u,
                [=](PassHandle self, Renderer* r)
                {
                    r->BindTextureSampler(self, srcSampler, "sampler");
                    r->BindShader(self, RHIShaderStageBits::Compute, "csMain", Foundation::Core::PathsResolve("Data/Shaders/CSMipGeneration.spv"));
                    uint32_t dstMipLevel = i;
                    if (src != dst)
                        dstMipLevel--;
                    if (i == 1)
                        r->BindTextureSRV(self, src, "srcTexture", RHIPipelineStageBits::ComputeShader,
                                          {.format = srcFormat,
                                           .range = RHITextureSubresourceRange::Create(srcAspect, 0, 1, layer, 1)});
                    else
                        r->BindTextureSRV(
                            self, dst, "srcTexture", RHIPipelineStageBits::ComputeShader,
                            {.format = dstFormat,
                             .range = RHITextureSubresourceRange::Create(dstAspect, dstMipLevel - 1, 1, layer, 1)});
                    r->BindTextureUAV(
                        self, dst, "dstTexture", RHIPipelineStageBits::ComputeShader,
                        {.format = dstFormat,
                         .range = RHITextureSubresourceRange::Create(dstAspect, dstMipLevel, 1, layer, 1)});
                    r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(float2));
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    RHITexture* dstTex = r->DerefResource(dst).Get<RHITexture*>();
                    RHIExtent3D extent = dstTex->mDesc.extent;
                    r->CmdSetPipeline(self, cmd);
                    uint32_t mipIndex = i;
                    if (src != dst)
                        mipIndex--;
                    uint32_t w = std::max(1u, extent.x >> mipIndex);
                    uint32_t h = std::max(1u, extent.y >> mipIndex);
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, float2{w, h});
                    r->CmdDispatch(self, cmd, {w, h, 1});
                });
        }
    }
    /**
     * @brief Generates full mip-chain with a single compute dispatch.
     * @note When src != dst, the first *generated* mip (i.e. mip 1 of the src) is written to the dst's mip *0*.
     */
    inline void createCSMipGenerationSinglePass(
        Renderer* renderer, StringView name, RHIDeviceQueueType queue, ResourceHandle src, ResourceHandle dst,
        RHIResourceFormat srcFormat, RHIResourceFormat dstFormat, RHITextureAspectFlag srcAspect,
        RHITextureAspectFlag dstAspect, ResourceHandle srcSampler, uint32_t numMips, uint32_t numLayer = 1,
        RHIDeviceSampler::SamplerDesc::Reduction reduction = RHIDeviceSampler::SamplerDesc::Reduction::WeightedAverage)
    {
        using namespace Math;
        struct PushConstants
        {
            uint2 extents;
            uint32_t mips;
            uint32_t numWorkGroups;
        };
        // From ffx_spd.h
        auto SpdSetup = [](uint2& dispatchThreadGroupCountXY, // CPU side: dispatch thread group count xy
                           uint32_t& numWorkGroups, // GPU side: pass in as constant
                           uint2 extent // width, height
                        )
        {
            uint32_t endIndexX = (extent[0] - 1) / 64; // rectInfo[0] = left, rectInfo[2] = width
            uint32_t endIndexY = (extent[1] - 1) / 64; // rectInfo[1] = top, rectInfo[3] = height

            dispatchThreadGroupCountXY[0] = endIndexX + 1;
            dispatchThreadGroupCountXY[1] = endIndexY + 1;

            numWorkGroups = (dispatchThreadGroupCountXY[0]) * (dispatchThreadGroupCountXY[1]);
        };
        auto SpdCounter = renderer->CreateResource(
            fmt::format("{} SPD Atomics", name),
            RHIBufferDesc{
                .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                .size = sizeof(uint32_t) * 6,
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
                r->BindShader(self, RHIShaderStageBits::Compute, "csMain", Foundation::Core::PathsResolve("Data/Shaders/CSMipGenerationSinglePass.spv"),
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
                for (uint32_t mip = 1; mip <= 12; mip++)
                {
                    uint32_t dstMipLevel = mip;
                    if (src != dst)
                        dstMipLevel--;
                    dstMipLevel = std::min(dstMipLevel, numMips - 1);
                    r->BindTextureUAV(
                        self, dst, "imgDst", RHIPipelineStageBits::ComputeShader,
                        {
                            .format = dstFormat,
                            .dimension = RHITextureDimension::E2DArray,
                            .range = RHITextureSubresourceRange::Create(dstAspect, dstMipLevel, 1, 0, numLayer),
                        });
                    if (mip == 6)
                        r->BindTextureUAV(
                            self, dst, "imgDst6", RHIPipelineStageBits::ComputeShader,
                            {
                                .format = dstFormat,
                                .dimension = RHITextureDimension::E2DArray,
                                .range = RHITextureSubresourceRange::Create(dstAspect, dstMipLevel, 1, 0, numLayer),
                            });
                }
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                auto* dstTex = r->DerefResource(dst).Get<RHITexture*>();
                PushConstants pc{.extents = {dstTex->mDesc.extent.x, dstTex->mDesc.extent.y}, .mips = numMips};
                if (src != dst) // Work starts from src mip 0
                    pc.extents *= 2, pc.mips++;
                uint2 dispatchThreadGroupCountXY;
                SpdSetup(dispatchThreadGroupCountXY, pc.numWorkGroups, pc.extents);
                if (r->GetFrame() == 0)
                {
                    auto* ctr = r->DerefResource(SpdCounter).Get<RHIBuffer*>();
                    cmd->FillBuffer(ctr, 0u);
                    cmd->BeginTransition();
                    cmd->SetBufferTransition(
                        ctr,
                        {
                            .srcAccess = RHIResourceAccessBits::TransferWrite,
                            .dstAccess = RHIResourceAccessBits::ShaderWrite,
                            .srcStage = RHIPipelineStageBits::ComputeShader | RHIPipelineStageBits::Transfer,
                            .dstStage = RHIPipelineStageBits::ComputeShader,
                        });
                    cmd->EndTransition();
                }
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, pc);
                cmd->Dispatch(dispatchThreadGroupCountXY.x, dispatchThreadGroupCountXY.y, 1);
            });
    }
} // namespace Foundation::RenderUtils
