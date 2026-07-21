#pragma once
#include <Math/Math.hpp>
#include <RenderCore/RenderPass.hpp>
using namespace Foundation;
using namespace Foundation::Math;
using namespace Foundation::RenderCore;
#pragma pack(push, 4)
struct RendererUBO
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
    float4x4 previousViewProj;
    uint32_t cameraHistoryFrame{UINT32_MAX};
    float4 projPlanes; // ij:left, kl:top
    float aperture{0.0f}; // Aperture radius in world units
    float focalDistance{1e1};
    uint32_t apertureBlades{0u};
    float apertureRotation{0.0f};
    float apertureRatio{1.0f};
    uint32_t cameraProjection{0u};
    // -- Framebuffers
    // Note: ALWAYS updated by Renderers themselves (PT/RASTER)    
    float fbWidth;
    float fbHeight;
    uint32_t hizLevels;
    uint32_t hizWidth;
    uint32_t hizHeight;
    // -- Lighting
    float camEV{0.0f};
    float4 camPosition;
    float4 camDirection;
    uint32_t ggxLutEIndex{UINT32_MAX};
    uint32_t ggxLutEavgIndex{UINT32_MAX};
    uint32_t ggxLutEIORIndex{UINT32_MAX};
    uint32_t ggxLutEIORavgIndex{UINT32_MAX};
    uint32_t ggxLutEIORInvIndex{UINT32_MAX};
    uint32_t ggxLutEIORInvavgIndex{UINT32_MAX};
    uint32_t sheenLtcIndex{UINT32_MAX};
    uint32_t envMapTextureIndex{UINT32_MAX};
    uint32_t envMapMarginalCDFIndex{UINT32_MAX};
    uint32_t envMapConditionalCDFIndex{UINT32_MAX};
    uint32_t envMapPrefilteredMips{0u};
    uint32_t hasEnvMap{0u};
    float3 envSHCoeffs[9]{};
    uint32_t matcapTextureIndex{UINT32_MAX};
    // Scene lights
    uint32_t firstLight{0u};
    uint32_t numSceneLights{0u};
    uint32_t firstLightBVHNode{0u};
    uint32_t numLightBVHNodes{0u};
    uint32_t firstLightBVHLightIndex{0u};
    uint32_t numLightBVHLightIndices{0u};
    uint32_t firstLightBVHBitmask{0u};
    uint32_t firstLightBVHGlobalIndex{0u};
    uint32_t numLightBVHGlobalLights{0u};
    uint32_t lightBVHValid{0u};
    uint32_t firstLightBVHDistantNode{0u};
    uint32_t numLightBVHDistantNodes{0u};
    uint32_t energyCompensation{1u};
    // -- Path Tracing
    uint32_t ptAccumulatedFrames{0u};
    uint32_t ptMaxBouncesDiffuse{4u};
    uint32_t ptMaxBouncesSpecular{4u};
    uint32_t ptMaxBouncesTransmission{12u};
    float ptFireflyClamp{2.0f}; // 10^x
    uint32_t ptSamplesPerPixel{1u}; // Always >= 1.
    uint32_t ptPrimaryLightVisibility{0u}; // Analytic lights + sun disks on bounce 0
    // -- Debug
    uint32_t dbgShowOutline{0u};
    uint32_t dbgViewFlags{0u};
    uint32_t dbgMaterialFlags{0u};
    uint32_t adaptiveMinSamples{32u};
    float adaptiveThreshold{0.10f}; // 0 = disabled
    float rasterRTShadowBias{0.01f};
};
#pragma pack(pop)

inline void UpdateRendererCameraUBO(RendererUBO& ubo, uint32_t frameNumber, float4x4 const& view,
                                    float4x4 const& proj)
{
    if (ubo.cameraHistoryFrame != frameNumber)
    {
        ubo.previousViewProj =
            ubo.cameraHistoryFrame == UINT32_MAX ? proj * view : ubo.proj * ubo.view;
        ubo.cameraHistoryFrame = frameNumber;
    }
    ubo.frameNumber = frameNumber;
    ubo.view = view;
    ubo.proj = proj;
    ubo.inverseView = inverse(view);
    ubo.inverseViewProj = inverse(proj * view);
}


static const int kViewOverdraw = 1 << 0;
static const int kViewMeshlet = 1 << 1;
static const int kViewBaseColor = 1 << 2;
static const int kViewNormal = 1 << 3;

static const int kViewPosition = 1 << 5;
static const int kViewMatcap = 1 << 6;
static const int kViewTextureLOD = 1 << 7;

// Per-lobe AOV view flags (Diffuse / Specular)
static const int kViewAOVDiffuse  = 1 << 8;
static const int kViewAOVSpecular = 1 << 9;
static const int kViewAOVSampleCount = 1 << 10;

// Material debug flags
static const int kMaterialDbgWhiteBaseColor = 1 << 0;

static const int kEnableRasterRTShadows = 1 << 16;
static const int kEnableRasterAmbientOcclusion = 1 << 17;
static const int kForceTextureLOD0 = 1 << 24;

static const int kCullFrustum = 1 << 0;
static const int kCullOcclusion = 1 << 1;
static const int kCullBackface = 1 << 2;

static const int kCullStageEarly = 1 << 16;
static const int kCullStageLate = 1 << 17;

static constexpr uint32_t kPTSamplerPCG = 0u;
static constexpr uint32_t kPTSamplerSobol = 1u;

static constexpr uint32_t kLightSamplerBVH = 0u;
static constexpr uint32_t kLightSamplerUniform = 1u;

static constexpr uint32_t kPTCompileOptionSamplerSobol = 1u << 1;
static constexpr uint32_t kPTCompileOptionSamplerPCG = 1u << 2;
static constexpr uint32_t kPTCompileOptionForceTextureLOD0 = 1u << 3;
static constexpr uint32_t kPTCompileOptionLightSamplerUniform = 1u << 4;
static constexpr uint32_t kPTCompileOptionEnergyCompensation = 1u << 5;

static constexpr uint32_t kCameraProjectionPerspective = 0u;
static constexpr uint32_t kCameraProjectionPanoramic = 1u;

inline uint32_t PTPackCompileOptions(uint32_t sampler, bool forceTextureLOD0, uint32_t lightSamplerMode,
                                    bool energyCompensation)
{
    uint32_t options = 0u;
    options |= sampler == kPTSamplerPCG ? kPTCompileOptionSamplerPCG : kPTCompileOptionSamplerSobol;
    options |= forceTextureLOD0 ? kPTCompileOptionForceTextureLOD0 : 0u;
    options |= lightSamplerMode == kLightSamplerUniform ? kPTCompileOptionLightSamplerUniform : 0u;
    options |= energyCompensation ? kPTCompileOptionEnergyCompensation : 0u;
    return options;
}

class GPUScene;

struct RendererConfig;

enum class RasterInjectionPoint : uint8_t
{
    AfterGBuffer,
    BeforeLighting,
    AfterLighting,
    BeforePostprocess,
};

struct RasterEffectContext
{
    Renderer* renderer{nullptr};
    RendererUBO* globals{nullptr};
    GPUScene* gpu{nullptr};
    RendererConfig const* cfg{nullptr};
    RHIExtent2D extent{0u, 0u};
    ResourceHandle globalUBO{kInvalidHandle};
    ResourceHandle primitiveBuffer{kInvalidHandle};
    ResourceHandle dynamicPrimitiveBuffer{kInvalidHandle};
    ResourceHandle instanceBuffer{kInvalidHandle};
    ResourceHandle materialBuffer{kInvalidHandle};
    ResourceHandle lightBuffer{kInvalidHandle};
    ResourceHandle tlas{kInvalidHandle};
    ResourceHandle gbuffer0{kInvalidHandle};
    ResourceHandle gbuffer1{kInvalidHandle};
    ResourceHandle gbuffer2{kInvalidHandle};
    ResourceHandle depth{kInvalidHandle};
    ResourceHandle instanceID{kInvalidHandle};
    ResourceHandle motionVectors{kInvalidHandle};
    ResourceHandle hiz{kInvalidHandle};
    ResourceHandle hizSampler{kInvalidHandle};
    ResourceHandle diffuse{kInvalidHandle};
    ResourceHandle specular{kInvalidHandle};
    ResourceHandle ambientOcclusion{kInvalidHandle};
};

using RasterEffectCallback = void (*)(RasterEffectContext& ctx, void const* config);

struct RasterEffect
{
    RasterInjectionPoint injectionPoint{RasterInjectionPoint::BeforeLighting};
    int order{0};
    RasterEffectCallback callback{nullptr};
    void const* config{nullptr};
};

struct RendererConfig
{
    unsigned viewFlags{kEnableRasterRTShadows};
    unsigned materialFlags{0u};
    unsigned cullFlags{kCullFrustum | kCullOcclusion | kCullBackface};
    RHIExtent2D renderExtent{0u, 0u};
    Span<RasterEffect const> rasterEffects{};
    uint32_t ptSampler{kPTSamplerSobol};
    uint32_t lightSamplerMode{kLightSamplerBVH};
    bool const* ptRenderPaused{nullptr};
    bool ptShaderExecutionReordering{true};
    bool forceTextureLOD0{false};
    bool energyCompensation{true};
    bool ptPrimaryLightVisibility{false};
    bool textureAnisoEnable{true};
    float textureAnisoLevel{16.0f};
    bool textureTrilinear{true};
    bool isRendering{false};
};

inline RHIDeviceSampler::SamplerDesc MakeTextureSamplerDesc(RendererConfig const& cfg)
{
    using SamplerDesc = RHIDeviceSampler::SamplerDesc;
    using MipmapMode = SamplerDesc::Mipmap::MipmapMode;
    return {
        .anisotropy = {.enable = cfg.textureAnisoEnable, .maxLevel = cfg.textureAnisoLevel},
        .mipmap = {.mipmapMode = cfg.textureTrilinear ? MipmapMode::Linear : MipmapMode::Nearest},
        .lod = {.max = 16.0f},
    };
}

struct RendererOutputs
{
    RHIExtent2D extent{0u, 0u};
    RHIResourceFormat aovFormat{RHIResourceFormat::R16G16B16A16SignedFloat};
    // Unified pre-postprocess outputs.
    ResourceHandle diffuse{kInvalidHandle};
    ResourceHandle specular{kInvalidHandle};
    ResourceHandle depth{kInvalidHandle};
    ResourceHandle instanceID{kInvalidHandle};
    ResourceHandle debugOutput{kInvalidHandle};
};

struct PostprocessUBO
{
    float camEV{0.0f};
    uint32_t viewLutIndex{UINT32_MAX};
    uint32_t dbgShowOutline{0u};
    uint32_t ptAccumulatedFrames{0u};
    float fbWidth{1.0f};
    float fbHeight{1.0f};
    uint32_t outlineInstanceId{~0u};
    float renderWidth{1.0f};   // Internal render target width
    float renderHeight{1.0f};  // Internal render target height
    uint32_t dbgViewFlags{0u};
};

extern void BuildRasterRenderGraph(Renderer* renderer, RendererUBO* globals, GPUScene* gpu,
                                   RendererConfig const& cfg, RendererOutputs& out);
extern void BuildPathTracerRenderGraph(Renderer* renderer, RendererUBO* globals, GPUScene* gpu,
                                       RendererConfig const& cfg, RendererOutputs& out);