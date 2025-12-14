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
    float4x4 inverseViewProj;
    float4 projPlanes; // ij:left, kl:top
    float fbWidth;
    float fbHeight;
    uint32_t hizLevels;
    uint32_t hizWidth;
    uint32_t hizHeight;
    // -- Lighting
    float camMinEV{4.5f};
    float camMaxEV{16.0f};
    float camAdaptCoeff; // 1 - exp(-dt * tau)
    float3 camPosition;
    float3 camDirection;
    float3 sunDirection{0, 0, -1};
    float sunIntensity{120'000.0f};
    float3 ambientColor{0,0,0};
};
#pragma pack(pop)

static const int kViewOverdraw = 1 << 0;
static const int kViewMeshlet = 1 << 1;
static const int kViewBaseColor = 1 << 2;
static const int kViewNormal = 1 << 3;
static const int kViewMaterialID = 1 << 4;
static const int kViewPosition = 1 << 5;
static const int kViewGBufferDiffuse = 1 << 6;
static const int kViewGBufferSpecular = 1 << 7;

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
    uint32_t* gsBLASNumPrimitives;
};
extern void RendererSetup(FContext* context, RendererConfig cfg, RendererScene scene);
