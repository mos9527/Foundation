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
    float3 ambientColor{1,1,1};
    float ambientPower{0.05f};
    uint32_t useEnvMap{0u};
    float envMapScale{1.0f};
    float envAzimuthOffset{0.0f}; // [-180, 180]
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
    uint32_t ptSampler{1u}; // 0: PCG, 1: Sobol
    // -- Display
    uint32_t enableHDR{0u};
    float paperWhiteNits{100.0f};
    // -- Debug
    uint32_t postShowOutline{0u};
};
#pragma pack(pop)

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

static const int kEnableRasterRTShadows = 1 << 16;

static const int kCullFrustum = 1 << 0;
static const int kCullOcclusion = 1 << 1;
static const int kCullBackface = 1 << 2;

static const int kCullStageEarly = 1 << 16;
static const int kCullStageLate = 1 << 17;

extern void RendererSetupImGuiOnly(FContext* context);

struct RendererConfig
{
    unsigned viewFlags{kEnableRasterRTShadows};
    unsigned cullFlags{kCullFrustum | kCullOcclusion | kCullBackface};
};

struct RendererScene
{
    UBO* gsGlobals;
    Vector<GSInstance>* gsInstances;
    Vector<GSMaterial>* gsMaterials;
    Vector<GSMesh>* gsMeshes;
    Vector<uint32_t>* gsBLASes;
    Vector<GSLight>* gsLights;
    int2* gsPickPixel; // Points to sPendingPickPixel in Editor.cpp; (-1,-1) = no pending pick
};

/**
 * @brief Editor-side readback resources shared by both renderer paths.
 *
 * hdrColor receives one or more RGBA32F lighting textures. The editor sums them
 * before writing HDR/EXR, so PT can export diffuse + specular while Raster exports
 * its single lighting buffer.
 */
struct RenderReadbackHandles
{
    ResourceHandle hdrColor[2]{kInvalidHandle, kInvalidHandle};
    uint32_t hdrColorCount{0u};
    ResourceHandle pickResultBuffer{kInvalidHandle}; // R32_UINT, 4 bytes, persistently mapped
};

extern void RendererSetup(FContext* context, RendererConfig cfg, RendererScene scene, RenderReadbackHandles& outHandles);
// Convenience overload — discards readback handles
inline void RendererSetup(FContext* context, RendererConfig cfg, RendererScene scene)
{
    RenderReadbackHandles dummy;
    RendererSetup(context, cfg, scene, dummy);
}

extern void PathTracerSetup(FContext* context, RendererConfig cfg, RendererScene scene, RenderReadbackHandles& outHandles);