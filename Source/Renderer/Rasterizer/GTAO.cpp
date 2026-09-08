#include "GTAO.hpp"

constexpr RHIResourceFormat kGBufferNormalFormat = RHIResourceFormat::A2B10G10R10Unorm;
constexpr RHIResourceFormat kAOFormat = RHIResourceFormat::R16Unorm;

struct GTAOPushConstants
{
    float radiusPixels;
    float radiusWorld;
    float intensity;
    float bias;
    uint32_t directionCount;
    uint32_t stepCount;
    uint32_t historyValid;
};

void GTAOFeatureCallback(RasterFeatureContext& ctx, void const* configPtr)
{
    CHECK(configPtr);
    auto* config = static_cast<GTAOConfig const*>(configPtr);
    auto* renderer = ctx.renderer;
    CHECK(ctx.globalUBO.IsValid() && ctx.gbuffer1.IsValid() && ctx.depth.IsValid());
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
            r->BindShader(self, RHIShaderStageBits::Compute, "main",
                          r->GetApplication()->ResolveRelativePathBase("Data/Shaders/ECSGTAO.spv"));
            r->BindBufferUniform(self, ctx.globalUBO.Previous(), RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSRV(
                self, ctx.gbuffer1.Previous(), "RT1", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = kGBufferNormalFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(
                self, ctx.depth.Previous(), "depth", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindTextureUAV(self, HalfAO, "aoOutput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(GTAOPushConstants));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            GTAOPushConstants push{
                .radiusPixels = config->radiusPixels,
                .radiusWorld = config->radiusWorld,
                .intensity = config->intensity,
                .bias = config->bias,
                .directionCount = config->directionCount,
                .stepCount = config->stepCount,
                .historyValid = r->IsPreviousValid(ctx.globalUBO) && r->IsPreviousValid(ctx.depth) &&
                    r->IsPreviousValid(ctx.gbuffer1),
            };
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, push);
            r->CmdDispatch(self, cmd, {halfW, halfH, 1});
        });

    renderer->CreatePass(
        "GTAO Upsample", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main",
                          r->GetApplication()->ResolveRelativePathBase("Data/Shaders/ECSGTAOUpsample.spv"));
            r->BindTextureUAV(self, HalfAO, "halfAO", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(
                self, ctx.depth.Previous(), "depth", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindTextureUAV(self, AO, "aoOutput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(uint32_t));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            uint32_t historyValid = r->IsPreviousValid(ctx.globalUBO) && r->IsPreviousValid(ctx.depth) &&
                r->IsPreviousValid(ctx.gbuffer1);
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, historyValid);
            r->CmdDispatch(self, cmd, {w, h, 1});
        });

    ctx.ambientOcclusion = AO;
}
