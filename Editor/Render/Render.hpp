#pragma once
#include <RenderCore/RenderPass.hpp>
#pragma pack(push, 4)
struct UBO
{
    uint32_t frameNumber;
    uint32_t firstInstance;
    uint32_t numInstances;
    uint32_t firstMaterial;
    uint32_t numMaterials;
    float lodThreshold{0.005f};
    float zNear;
    float4x4 view;
    float4x4 proj;
    float4x4 inverseView;
    float4x4 inverseViewProj;
    float4 projPlanes; // ij:left, kl:top
    float aperture{0.0f}; // Aperture radius in world units
    float focalDistance{1e1};
    uint32_t apertureBlades{0u};
    float apertureRotation{0.0f};
    float apertureRatio{1.0f};
    // -- Framebuffers
    float fbWidth;
    float fbHeight;
    uint32_t hizLevels;
    uint32_t hizWidth;
    uint32_t hizHeight;
    // -- Lighting
    float camEV{0.0f};
    float4 camPosition;
    float4 camDirection;
    // Environment light
    float3 ambientColor{0.05f, 0.05f, 0.05f};
    float ambientPower{1.0f};
    uint32_t useEnvMap{0u};
    float envAzimuthOffset{0.0f}; // [-180, 180]
    uint32_t ggxLutEIndex{UINT32_MAX};
    uint32_t sheenLtcIndex{UINT32_MAX};
    uint32_t viewLutIndex{UINT32_MAX};
    uint32_t envMapTextureIndex{UINT32_MAX};
    uint32_t envMapMarginalCDFIndex{UINT32_MAX};
    uint32_t envMapConditionalCDFIndex{UINT32_MAX};
    uint32_t _envTexturePad0{0u};
    // Scene lights
    uint32_t firstLight{0u};
    uint32_t firstLightAliasTable{0u};
    uint32_t numSceneLights{0u};
    float sceneLightWeightSum{0.0f};
    uint32_t _lightPad0{0u};
    // -- Path Tracing
    uint32_t ptAccumulatedFrames{0u};
    uint32_t ptMaxBouncesDiffuse{4u};
    uint32_t ptMaxBouncesSpecular{4u};
    uint32_t ptMaxBouncesTransmission{12u};
    float ptFireflyClamp{1.0f}; // 10^x
    uint32_t ptSamplesPerPixel{1u}; // Always >= 1; fractional SPP uses ptDispatchTileSide instead.
    uint32_t ptDispatchTileSide{3u}; // 1 = full dispatch, n > 1 = 1/(n*n) tile dispatch.
    // -- Debug
    uint32_t postShowOutline{0u};
    uint32_t ptViewFlags{0u};
};
#pragma pack(pop)

inline uint32_t PTDispatchTileSide(UBO const& ubo)
{
    return ubo.ptDispatchTileSide > 0u ? ubo.ptDispatchTileSide : 1u;
}

inline uint32_t PTTileSampleCount(UBO const& ubo)
{
    uint32_t tileSide = PTDispatchTileSide(ubo);
    return tileSide * tileSide;
}

inline uint32_t PTSamplesPerDispatch(UBO const& ubo)
{
    return ubo.ptSamplesPerPixel > 0u ? ubo.ptSamplesPerPixel : 1u;
}

inline uint32_t PTCompletedPixelSamples(UBO const& ubo)
{
    return ubo.ptAccumulatedFrames / PTTileSampleCount(ubo);
}

inline uint32_t PTAccumulationStepTarget(UBO const& ubo, uint32_t targetSamples)
{
    return targetSamples * PTTileSampleCount(ubo);
}

inline uint32_t PTDispatchesForPixelSamples(UBO const& ubo, uint32_t targetSamples)
{
    uint32_t samplesPerDispatch = PTSamplesPerDispatch(ubo);
    uint32_t targetSteps = PTAccumulationStepTarget(ubo, targetSamples);
    return (targetSteps + samplesPerDispatch - 1u) / samplesPerDispatch;
}

static const int kViewOverdraw = 1 << 0;
static const int kViewMeshlet = 1 << 1;
static const int kViewBaseColor = 1 << 2;
static const int kViewNormal = 1 << 3;
static const int kViewMaterialID = 1 << 4;
static const int kViewPosition = 1 << 5;
static const int kViewPTDirect = 1 << 6;

// Per-lobe AOV view flags (Diffuse / Specular)
static const int kViewAOVDiffuse  = 1 << 7;
static const int kViewAOVSpecular = 1 << 8;
static const int kViewPTRayDX     = 1 << 9;

static const int kEnableRasterRTShadows = 1 << 16;

static const int kCullFrustum = 1 << 0;
static const int kCullOcclusion = 1 << 1;
static const int kCullBackface = 1 << 2;

static const int kCullStageEarly = 1 << 16;
static const int kCullStageLate = 1 << 17;

static constexpr uint32_t kPTSamplerPCG = 0u;
static constexpr uint32_t kPTSamplerSobol = 1u;

static constexpr uint32_t kPTCompileOptionShaderExecutionReordering = 1u << 0;
static constexpr uint32_t kPTCompileOptionSamplerSobol = 1u << 1;
static constexpr uint32_t kPTCompileOptionSamplerPCG = 1u << 2;

inline uint32_t PTPackCompileOptions(bool shaderExecutionReordering, uint32_t sampler)
{
    uint32_t options = shaderExecutionReordering ? kPTCompileOptionShaderExecutionReordering : 0u;
    options |= sampler == kPTSamplerPCG ? kPTCompileOptionSamplerPCG : kPTCompileOptionSamplerSobol;
    return options;
}

struct RendererConfig
{
    unsigned viewFlags{kEnableRasterRTShadows};
    unsigned cullFlags{kCullFrustum | kCullOcclusion | kCullBackface};
    uint32_t ptSampler{kPTSamplerSobol};
    bool ptShaderExecutionReordering{true};
};

struct RendererPicking
{
    int2 pendingPixel{-1, -1}; // (-1,-1) = no pending pick
};

struct RendererScene
{
    UBO* gsGlobals;
    Vector<GSInstance>* gsInstances;
    Vector<uint32_t>* gsBLASes;
    Vector<uint32_t>* gsCurveBLASes;
    Vector<GSLight>* gsLights;
    RendererPicking* picking;
    bool* rendererRebuildRequested{nullptr};
};

struct RendererHandles
{
    ResourceHandle hdrRT[2]{kInvalidHandle, kInvalidHandle};
    uint32_t numHdrRT{0u};
    ResourceHandle sdrRT{kInvalidHandle};
    ResourceHandle pickBuffer{kInvalidHandle}; // R32_UINT, 4 bytes, persistently mapped

};

extern void BuildIdleRenderGraph(FContext* context, float const* timeSeconds);
extern void BuildRasterRenderGraph(FContext* context, RendererConfig cfg, RendererScene scene, RHIExtent2D renderExtent,
                                   RendererHandles& outHandles);
extern void BuildPathTracerRenderGraph(FContext* context, RendererConfig cfg, RendererScene scene, RHIExtent2D renderExtent,
                                       RendererHandles& outHandles, bool const* renderPaused);