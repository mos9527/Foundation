#include "TAA.hpp"
#include <algorithm>

namespace
{
constexpr RHIResourceFormat kTAAFormat = RHIResourceFormat::R16G16B16A16SignedFloat;

struct TAAPushConstants
{
    float historyWeight;
    uint32_t historyValid;
    uint32_t neighborhoodClamp;
    uint32_t padding;
};
} // namespace

void TAAFeatureCallback(RasterFeatureContext& ctx, void const* configPtr)
{
    CHECK(configPtr);
    CHECK(ctx.renderer && ctx.globals);
    CHECK(ctx.globalUBO.IsValid());
    CHECK(ctx.diffuse != kInvalidHandle && ctx.specular != kInvalidHandle);

    auto const* config = static_cast<TAAConfig const*>(configPtr);
    auto* renderer = ctx.renderer;
    ctx.globals->accumulatedFrames = 0u;
    uint32_t const w = ctx.extent.x;
    uint32_t const h = ctx.extent.y;

    auto DiffuseHistory = renderer->CreateTemporalResource(
        "TAA Diffuse History",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = kTAAFormat});
    auto SpecularHistory = renderer->CreateTemporalResource(
        "TAA Specular History",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = kTAAFormat});

    renderer->CreatePass(
        "TAA Resolve", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main",
                          r->GetApplication()->ResolveRelativePathBase("Data/Shaders/ECSTAA.spv"));
            RHITextureViewDesc const view{.format = kTAAFormat, .range = RHITextureSubresourceRange::Create()};
            r->BindTextureSRV(self, ctx.diffuse, "currentDiffuse", RHIPipelineStageBits::ComputeShader, view);
            r->BindTextureSRV(self, ctx.specular, "currentSpecular", RHIPipelineStageBits::ComputeShader, view);
            r->BindTextureSRV(self, DiffuseHistory.Previous(), "historyDiffuse",
                              RHIPipelineStageBits::ComputeShader, view);
            r->BindTextureSRV(self, SpecularHistory.Previous(), "historySpecular",
                              RHIPipelineStageBits::ComputeShader, view);
            r->BindTextureUAV(self, DiffuseHistory.Current(), "outputDiffuse", RHIPipelineStageBits::ComputeShader,
                              view);
            r->BindTextureUAV(self, SpecularHistory.Current(), "outputSpecular", RHIPipelineStageBits::ComputeShader,
                              view);
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(TAAPushConstants));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            bool const historyValid = ctx.globals->accumulatedFrames > 0u &&
                r->IsPreviousValid(DiffuseHistory) && r->IsPreviousValid(SpecularHistory);
            TAAPushConstants const push{
                .historyWeight = std::clamp(config->historyWeight, 0.0f, 0.99f),
                .historyValid = historyValid ? 1u : 0u,
                .neighborhoodClamp = config->neighborhoodClamp ? 1u : 0u,
                .padding = 0u,
            };
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, push);
            r->CmdDispatch(self, cmd, {w, h, 1});
        });

    ctx.diffuse = DiffuseHistory.Current();
    ctx.specular = SpecularHistory.Current();
}
