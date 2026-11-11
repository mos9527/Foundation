#include "PathtracerDenoiser.hpp"
#include <algorithm>
#include <array>

namespace
{
constexpr uint32_t kDenoiseIterations = 3u;
constexpr uint32_t kDenoiseDebugViews =
    to_integer(ViewFlagsBits::BaseColor) | to_integer(ViewFlagsBits::Normal) |
    to_integer(ViewFlagsBits::Position) | to_integer(ViewFlagsBits::TextureLOD) |
    to_integer(ViewFlagsBits::SHARCGrid) | to_integer(ViewFlagsBits::SHARCOccupancy) |
    to_integer(ViewFlagsBits::SHARCRadiance);

struct PTDenoisePushConstants
{
    uint32_t stepWidth;
    uint32_t enabled;
    float normalPower;
    float relativeDepthSigma;
    float albedoSigma;
    float luminanceSigma;
};
static_assert(sizeof(PTDenoisePushConstants) == 24u);

// These layouts are mirrored in ECSPTTemporalDenoise.slang (column-major, scalar layout).
struct PTDenoiseFrameConstants
{
    float4x4 view;
    float4x4 inverseView;
    float4 cameraPosition;
    float2 cameraTanHalfFov;
    uint32_t frameNumber;
    uint32_t historyEpoch;
    uint32_t supported;
    uint32_t resetHistory;
    uint32_t sampleNumber;
    uint32_t padding;
};
static_assert(sizeof(PTDenoiseFrameConstants) == 176u);

struct PTTemporalDenoisePushConstants
{
    uint32_t historyValid;
    uint32_t maxDiffuseFrames;
    uint32_t maxSpecularFrames;
    float relativeDepthThreshold;
    float normalThreshold;
    float albedoThreshold;
    float clampSigma;
    uint32_t padding;
};
static_assert(sizeof(PTTemporalDenoisePushConstants) == 32u);

bool DenoiseSettingsChanged(RendererUBO const& current, RendererUBO const& previous)
{
    // Deliberately exclude view/position and accumulatedFrames: ordinary camera movement reprojects.
    return current.cameraProjection != previous.cameraProjection || current.aperture != previous.aperture ||
        current.cameraTanHalfFov.x != previous.cameraTanHalfFov.x ||
        current.cameraTanHalfFov.y != previous.cameraTanHalfFov.y || current.zNear != previous.zNear ||
        current.dbgMaterialFlags != previous.dbgMaterialFlags || current.ptMaxBounces != previous.ptMaxBounces ||
        current.ptFireflyClamp != previous.ptFireflyClamp ||
        current.ptPrimaryLightVisibility != previous.ptPrimaryLightVisibility ||
        current.energyCompensation != previous.energyCompensation || current.sharcEnabled != previous.sharcEnabled ||
        current.hasEnvMap != previous.hasEnvMap || current.envMapTextureIndex != previous.envMapTextureIndex;
}

void BuildPTTemporalDenoisePass(Renderer* renderer, RendererUBO const* globals, RendererConfig const& cfg,
                               PTDenoiserGuides const& guides, RendererOutputs& outputs)
{
    if (!cfg.ptDenoiseTemporal || !guides.temporalSupported)
        return;

    auto const extent = outputs.extent;
    auto const format = outputs.aovFormat;
    auto createHistory = [&](char const* name, RHIResourceFormat historyFormat)
    {
        return renderer->CreateTemporalResource(
            name, RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                                 .extent = {extent.x, extent.y, 1u},
                                 .format = historyFormat});
    };
    auto DiffuseHistory = createHistory("PT Temporal Diffuse", format);
    auto SpecularHistory = createHistory("PT Temporal Specular", format);
    auto NormalHistory = createHistory("PT Temporal Normal Roughness", kPTDenoiserGuideFormat);
    auto AlbedoHistory = createHistory("PT Temporal Albedo Metallic", kPTDenoiserGuideFormat);
    auto DepthHistory = createHistory("PT Temporal Linear Depth", kPTDenoiserDepthFormat);
    auto InstanceHistory = createHistory("PT Temporal Instance ID", RHIResourceFormat::R32Uint);
    auto FrameHistory = renderer->CreateTemporalResource(
        "PT Temporal Camera",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(PTDenoiseFrameConstants)});

    renderer->CreatePass(
        "PT Temporal Camera Update", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r) { r->BindBufferCopyDst(self, FrameHistory.Current()); },
        [=, previousGlobals = RendererUBO{}, frame = PTDenoiseFrameConstants{}, initialized = false]
        (PassHandle, Renderer* r, RHICommandList* cmd) mutable
        {
            // Match the trace pass's first-frame/pause policy. Each callback owns its state;
            // render-pass recording may run concurrently, so there is no shared mutable flag.
            bool const canTrace = !initialized || !cfg.ptRenderPaused || !*cfg.ptRenderPaused;
            if (canTrace)
            {
                uint32_t const sampleNumber = frame.sampleNumber + 1u;
                frame = {
                    .view = globals->view,
                    .inverseView = globals->inverseView,
                    .cameraPosition = globals->camPosition,
                    .cameraTanHalfFov = globals->cameraTanHalfFov,
                    .frameNumber = globals->frameNumber,
                    .historyEpoch = globals->ptDenoiseHistoryEpoch,
                    .supported = globals->cameraProjection == to_integer(CameraProjection::Perspective) &&
                        globals->aperture <= 0.0f && globals->cameraTanHalfFov.x > 0.0f &&
                        globals->cameraTanHalfFov.y > 0.0f && (globals->dbgViewFlags & kDenoiseDebugViews) == 0u
                        ? 1u : 0u,
                    .resetHistory = !initialized || DenoiseSettingsChanged(*globals, previousGlobals) ? 1u : 0u,
                    .sampleNumber = sampleNumber,
                    .padding = 0u,
                };
                previousGlobals = *globals;
                initialized = true;
            }
            // Freeze the camera/seed with the image, not with display frames. This also makes
            // the first resumed frame reproject from the last actually traced camera.
            cmd->UpdateBuffer(r->DerefResource(FrameHistory.Current()).Get<RHIBuffer*>(), 0, AsBytes(AsSpan(frame)));
        });

    ResourceHandle const currentDiffuse = outputs.diffuse;
    ResourceHandle const currentSpecular = outputs.specular;
    ResourceHandle const currentInstanceID = outputs.instanceID;
    renderer->CreatePass(
        "PT Temporal Resolve", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            constexpr auto stage = RHIPipelineStageBits::ComputeShader;
            r->BindShader(self, RHIShaderStageBits::Compute, "main",
                          r->GetApplication()->ResolveRelativePathBase("Data/Shaders/ECSPTTemporalDenoise.spv"));
            r->BindBufferUniform(self, FrameHistory.Current(), stage, "currentFrame");
            r->BindBufferUniform(self, FrameHistory.Previous(), stage, "previousFrame");
            RHITextureViewDesc const aovView{.format = format, .range = RHITextureSubresourceRange::Create()};
            RHITextureViewDesc const guideView{.format = kPTDenoiserGuideFormat,
                                               .range = RHITextureSubresourceRange::Create()};
            RHITextureViewDesc const depthView{.format = kPTDenoiserDepthFormat,
                                               .range = RHITextureSubresourceRange::Create()};
            RHITextureViewDesc const idView{.format = RHIResourceFormat::R32Uint,
                                            .range = RHITextureSubresourceRange::Create()};
            r->BindTextureSRV(self, currentDiffuse, "currentDiffuse", stage, aovView);
            r->BindTextureSRV(self, currentSpecular, "currentSpecular", stage, aovView);
            r->BindTextureSRV(self, guides.normalRoughness, "currentNormalRoughness", stage, guideView);
            r->BindTextureSRV(self, guides.albedoMetallic, "currentAlbedoMetallic", stage, guideView);
            r->BindTextureSRV(self, guides.linearDepth, "currentLinearDepth", stage, depthView);
            r->BindTextureSRV(self, currentInstanceID, "currentInstanceID", stage, idView);
            r->BindTextureSRV(self, DiffuseHistory.Previous(), "historyDiffuse", stage, aovView);
            r->BindTextureSRV(self, SpecularHistory.Previous(), "historySpecular", stage, aovView);
            r->BindTextureSRV(self, NormalHistory.Previous(), "historyNormalRoughness", stage, guideView);
            r->BindTextureSRV(self, AlbedoHistory.Previous(), "historyAlbedoMetallic", stage, guideView);
            r->BindTextureSRV(self, DepthHistory.Previous(), "historyLinearDepth", stage, depthView);
            r->BindTextureSRV(self, InstanceHistory.Previous(), "historyInstanceID", stage, idView);
            r->BindTextureUAV(self, DiffuseHistory.Current(), "outputDiffuse", stage, aovView);
            r->BindTextureUAV(self, SpecularHistory.Current(), "outputSpecular", stage, aovView);
            r->BindTextureUAV(self, NormalHistory.Current(), "outputNormalRoughness", stage, guideView);
            r->BindTextureUAV(self, AlbedoHistory.Current(), "outputAlbedoMetallic", stage, guideView);
            r->BindTextureUAV(self, DepthHistory.Current(), "outputLinearDepth", stage, depthView);
            r->BindTextureUAV(self, InstanceHistory.Current(), "outputInstanceID", stage, idView);
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PTTemporalDenoisePushConstants));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            bool const historyValid = r->IsPreviousValid(FrameHistory) && r->IsPreviousValid(DiffuseHistory) &&
                r->IsPreviousValid(SpecularHistory) && r->IsPreviousValid(NormalHistory) &&
                r->IsPreviousValid(AlbedoHistory) && r->IsPreviousValid(DepthHistory) && r->IsPreviousValid(InstanceHistory);
            PTTemporalDenoisePushConstants const push{
                .historyValid = historyValid ? 1u : 0u,
                .maxDiffuseFrames = std::clamp(cfg.ptDenoiseDiffuseHistory, 1u, 64u),
                .maxSpecularFrames = std::clamp(cfg.ptDenoiseSpecularHistory, 1u, 16u),
                .relativeDepthThreshold = std::clamp(cfg.ptDenoiseTemporalDepthThreshold, 0.0001f, 0.1f),
                .normalThreshold = std::clamp(cfg.ptDenoiseTemporalNormalThreshold, 0.0f, 0.9999f),
                .albedoThreshold = std::clamp(cfg.ptDenoiseAlbedoSigma, 0.001f, 4.0f),
                .clampSigma = std::clamp(cfg.ptDenoiseHistoryClampSigma, 0.5f, 8.0f),
                .padding = 0u,
            };
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, push);
            r->CmdDispatch(self, cmd, {extent.x, extent.y, 1u});
        });
    // Feed spatial filtering from the temporal resolve, never feed the wide spatial blur back into history.
    outputs.diffuse = DiffuseHistory.Current();
    outputs.specular = SpecularHistory.Current();
}
} // namespace

void BuildPTDenoisePasses(Renderer* renderer, RendererUBO const* globals, RendererConfig const& cfg,
                         PTDenoiserGuides const& guides, RendererOutputs& outputs)
{
    if (!cfg.ptDenoise)
        return;

    CHECK(renderer && globals);
    CHECK(outputs.diffuse != kInvalidHandle && outputs.specular != kInvalidHandle);
    CHECK(outputs.instanceID != kInvalidHandle);
    CHECK(guides.normalRoughness != kInvalidHandle && guides.albedoMetallic != kInvalidHandle &&
          guides.linearDepth != kInvalidHandle);
    CHECK(outputs.extent.x > 0u && outputs.extent.y > 0u);

    BuildPTTemporalDenoisePass(renderer, globals, cfg, guides, outputs);
    auto const extent = outputs.extent;
    auto const format = outputs.aovFormat;
    RHITextureDesc const desc{
        .usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage |
            RHITextureUsageBits::TransferSource | RHITextureUsageBits::RenderTarget,
        .extent = {extent.x, extent.y, 1u},
        .format = format,
    };
    std::array<ResourceHandle, 2> diffuseScratch;
    std::array<ResourceHandle, 2> specularScratch;
    for (uint32_t i = 0u; i < diffuseScratch.size(); ++i)
    {
        String const diffuseName = Format("PT Denoise Diffuse {}", i);
        String const specularName = Format("PT Denoise Specular {}", i);
        diffuseScratch[i] = renderer->CreateResource(diffuseName.c_str(), desc);
        specularScratch[i] = renderer->CreateResource(specularName.c_str(), desc);
    }

    ResourceHandle inputDiffuse = outputs.diffuse;
    ResourceHandle inputSpecular = outputs.specular;
    ResourceHandle const instanceID = outputs.instanceID;
    for (uint32_t iteration = 0u; iteration < kDenoiseIterations; ++iteration)
    {
        ResourceHandle const outputDiffuse = diffuseScratch[iteration % diffuseScratch.size()];
        ResourceHandle const outputSpecular = specularScratch[iteration % specularScratch.size()];
        uint32_t const stepWidth = 1u << iteration;
        String const passName = Format("PT Denoise A-trous {}", stepWidth);
        // Stay on the tracing queue; these are dependent compute dispatches, not async work.
        renderer->CreatePass(
            passName.c_str(), RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                constexpr auto stage = RHIPipelineStageBits::ComputeShader;
                r->BindShader(self, RHIShaderStageBits::Compute, "main",
                              r->GetApplication()->ResolveRelativePathBase("Data/Shaders/ECSPTDenoise.spv"));
                RHITextureViewDesc const aovView{.format = format, .range = RHITextureSubresourceRange::Create()};
                RHITextureViewDesc const guideView{.format = kPTDenoiserGuideFormat,
                                                   .range = RHITextureSubresourceRange::Create()};
                r->BindTextureSRV(self, inputDiffuse, "inputDiffuse", stage, aovView);
                r->BindTextureSRV(self, inputSpecular, "inputSpecular", stage, aovView);
                r->BindTextureSRV(self, guides.normalRoughness, "normalRoughness", stage, guideView);
                r->BindTextureSRV(self, guides.albedoMetallic, "albedoMetallic", stage, guideView);
                r->BindTextureSRV(self, guides.linearDepth, "linearDepth", stage,
                                  {.format = kPTDenoiserDepthFormat, .range = RHITextureSubresourceRange::Create()});
                r->BindTextureSRV(self, instanceID, "instanceID", stage,
                                  {.format = RHIResourceFormat::R32Uint,
                                   .range = RHITextureSubresourceRange::Create()});
                r->BindTextureUAV(self, outputDiffuse, "outputDiffuse", stage, aovView);
                r->BindTextureUAV(self, outputSpecular, "outputSpecular", stage, aovView);
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(PTDenoisePushConstants));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                PTDenoisePushConstants const push{
                    .stepWidth = stepWidth,
                    .enabled = (globals->dbgViewFlags & kDenoiseDebugViews) == 0u ? 1u : 0u,
                    .normalPower = std::clamp(cfg.ptDenoiseNormalPower, 1.0f, 256.0f),
                    .relativeDepthSigma = std::clamp(cfg.ptDenoiseRelativeDepthSigma, 0.0001f, 1.0f),
                    .albedoSigma = std::clamp(cfg.ptDenoiseAlbedoSigma, 0.001f, 4.0f),
                    .luminanceSigma = std::clamp(cfg.ptDenoiseLuminanceSigma, 0.01f, 16.0f),
                };
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, push);
                r->CmdDispatch(self, cmd, {extent.x, extent.y, 1u});
            });
        inputDiffuse = outputDiffuse;
        inputSpecular = outputSpecular;
    }
    outputs.diffuse = inputDiffuse;
    outputs.specular = inputSpecular;
}
