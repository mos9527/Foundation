#pragma once
#include <Math/Math.hpp>
#include <RenderCore/RenderPass.hpp>
using namespace Foundation;
using namespace Foundation::RenderCore;
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
    uint32_t ggxLutEavgIndex{UINT32_MAX};
    uint32_t ggxLutEIORIndex{UINT32_MAX};
    uint32_t ggxLutEIORavgIndex{UINT32_MAX};
    uint32_t ggxLutEIORInvIndex{UINT32_MAX};
    uint32_t ggxLutEIORInvavgIndex{UINT32_MAX};
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
    uint32_t energyCompensation{1u};
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
static const int kViewTextureLOD = 1 << 9;

static const int kEnableRasterRTShadows = 1 << 16;
static const int kForceTextureLOD0 = 1 << 24;

static const int kCullFrustum = 1 << 0;
static const int kCullOcclusion = 1 << 1;
static const int kCullBackface = 1 << 2;

static const int kCullStageEarly = 1 << 16;
static const int kCullStageLate = 1 << 17;

static constexpr uint32_t kPTSamplerPCG = 0u;
static constexpr uint32_t kPTSamplerSobol = 1u;

static constexpr uint32_t kPTCompileOptionSamplerSobol = 1u << 1;
static constexpr uint32_t kPTCompileOptionSamplerPCG = 1u << 2;
static constexpr uint32_t kPTCompileOptionForceTextureLOD0 = 1u << 3;

inline uint32_t PTPackCompileOptions(uint32_t sampler, bool forceTextureLOD0)
{
    uint32_t options = 0u;
    options |= sampler == kPTSamplerPCG ? kPTCompileOptionSamplerPCG : kPTCompileOptionSamplerSobol;
    options |= forceTextureLOD0 ? kPTCompileOptionForceTextureLOD0 : 0u;
    return options;
}

struct RendererConfig
{
    unsigned viewFlags{kEnableRasterRTShadows};
    unsigned cullFlags{kCullFrustum | kCullOcclusion | kCullBackface};
    uint32_t ptSampler{kPTSamplerSobol};
    bool ptShaderExecutionReordering{true};
    bool forceTextureLOD0{false};
    bool energyCompensation{true};
    bool enableHDR{false}; // Output color space: A2B10G10R10 vs R8G8B8A8
};

class GPUScene;

struct RendererPicking
{
    int2 pendingPixel{-1, -1}; // (-1,-1) = no pending pick
};

struct RendererScene
{
    // GPUScene owns all scene-data residency (geometry, instances, lights, materials);
    // the renderer only carries view/render config and picking state.
    UBO* gsGlobals;
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

extern void BuildIdleRenderGraph(Renderer* renderer, float const* timeSeconds);
extern void BuildRasterRenderGraph(Renderer* renderer, GPUScene* gpu, RendererConfig cfg, RendererScene scene,
                                   RHIExtent2D renderExtent, RendererHandles& outHandles);
extern void BuildPathTracerRenderGraph(Renderer* renderer, GPUScene* gpu, RendererConfig cfg, RendererScene scene,
                                       RHIExtent2D renderExtent, RendererHandles& outHandles, bool const* renderPaused);