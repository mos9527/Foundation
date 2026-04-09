#pragma once
#include "Scene.hpp"
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
    float aperture{0.0f};
    float focalDistance{1e1};
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
    float4 sunDirection{0, 0, -1, 0};
    float4 sunIntensity{0.0f};
    float4 ambientColor{0.25,0.25,0.25,0};
    uint32_t useEnvMap{0u};
    float envMapScale{1.0f};
    // -- Path Tracing
    uint32_t ptAccumulatedFrames{0u};
    uint32_t ptMaxBounces{32u};
    float ptFireflyClamp{10.0f};
    // -- Debug
    uint32_t postShowOutline{1u};
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

static const int kViewEnableRaytracing = 1 << 16;

static const int kCullFrustum = 1 << 0;
static const int kCullOcclusion = 1 << 1;
static const int kCullBackface = 1 << 2;

static const int kCullStageEarly = 1 << 16;
static const int kCullStageLate = 1 << 17;

extern void RendererSetupImGuiOnly(FContext* context);

struct RendererConfig
{
    unsigned viewFlags{kViewEnableRaytracing};
    unsigned cullFlags{kCullFrustum | kCullOcclusion | kCullBackface};
};

struct RendererScene
{
    UBO* gsGlobals;
    Vector<GSInstance>* gsInstances;
    Vector<GSMaterial>* gsMaterials;
    Vector<GSMesh>* gsMeshes;
    Vector<uint32_t>* gsBLASes;
    int2* gsPickPixel; // Points to GPendingPickPixel; (-1,-1) = no pending pick
};

/**
 * @brief Resource handles for the rasterizer path, for editor readback.
 */
struct RasterReadbackHandles
{
    ResourceHandle pickResultBuffer{kInvalidHandle}; // R32_UINT, 4 bytes, persistently mapped
};

extern void RendererSetup(FContext* context, RendererConfig cfg, RendererScene scene, RasterReadbackHandles& outHandles);
// Convenience overload — discards readback handles
inline void RendererSetup(FContext* context, RendererConfig cfg, RendererScene scene)
{
    RasterReadbackHandles dummy;
    RendererSetup(context, cfg, scene, dummy);
}

/**
 * @brief HDR resource handles holding the current accumulated path tracer result.
 * Set by PathTracerSetup; read by the editor at the end of each frame.
 */
struct PTReadbackHandles
{
    ResourceHandle diffuse{kInvalidHandle};
    ResourceHandle specular{kInvalidHandle};
    ResourceHandle sdrRenderTarget{kInvalidHandle};
    ResourceHandle pickResultBuffer{kInvalidHandle}; // R32_UINT, 4 bytes, persistently mapped
};

extern void PathTracerSetup(FContext* context, RendererConfig cfg, RendererScene scene, PTReadbackHandles& outHandles);
