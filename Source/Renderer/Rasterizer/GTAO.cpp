#include "GTAO.hpp"

constexpr RHIResourceFormat kGBufferNormalFormat = RHIResourceFormat::A2B10G10R10Unorm;
constexpr RHIResourceFormat kAOFormat = RHIResourceFormat::R16Unorm;

void GTAOFeatureCallback(RasterFeatureContext& ctx, void const* configPtr)
{
    CHECK(configPtr);
    auto* config = static_cast<GTAOConfig const*>(configPtr);
    auto* renderer = ctx.renderer;
    uint32_t w = ctx.extent.x;
    uint32_t h = ctx.extent.y;
    uint32_t halfW = std::max((w + 1u) / 2u, 1u);
    uint32_t halfH = std::max((h + 1u) / 2u, 1u);
    auto HalfAO = renderer->CreateResource(
        "GTAO Half",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {halfW, halfH, 1},
                       .format = kAOFormat});
    auto AO = renderer->CreateResource(
        "GTAO",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = kAOFormat});

    renderer->CreatePass(
        "GTAO", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSGTAO.spv"));
            r->BindBufferUniform(self, ctx.globalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSRV(
                self, ctx.gbuffer1, "RT1", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = kGBufferNormalFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(
                self, ctx.depth, "depth", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindTextureUAV(self, HalfAO, "aoOutput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(GTAOConfig));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, *config);
            r->CmdDispatch(self, cmd, {halfW, halfH, 1});
        });

    renderer->CreatePass(
        "GTAO Upsample", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSGTAOUpsample.spv"));
            r->BindBufferUniform(self, ctx.globalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSRV(self, HalfAO, "halfAO", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(
                self, ctx.depth, "depth", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindTextureUAV(self, AO, "aoOutput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOFormat, .range = RHITextureSubresourceRange::Create()});
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {w, h, 1});
        });

    ctx.ambientOcclusion = AO;
}
