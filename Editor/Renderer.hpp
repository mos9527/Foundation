#pragma once
#include "Scene.hpp"
#pragma pack(push, 1)
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
    float aperture{1e-3};
    float focalDistance{1e1};
    // -- Framebuffers
    float fbWidth;
    float fbHeight;
    uint32_t hizLevels;
    uint32_t hizWidth;
    uint32_t hizHeight;
    // -- Lighting
    float camEV{0.0f};
    float3 camPosition;
    float3 camDirection;
    float3 sunDirection{0, 0, -1};
    float3 sunIntensity{0.0f};
    float3 ambientColor{1,1,1};
    uint32_t useEnvMap{0u};
    float envMapScale{1.0f};
    // -- Path Tracing
    uint32_t ptAccumualatedFrames{0u};
    int32_t ptMaxBounces{32u};
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
};

extern void RendererSetup(FContext* context, RendererConfig cfg, RendererScene scene);
extern void PathTracerSetup(FContext* context, RendererConfig cfg, RendererScene scene);
