#pragma once
#include "Scene.hpp"
#pragma pack(push, 1)
struct UBO
{
    uint32_t firstInstance;
    uint32_t numInstances;
    uint32_t firstMaterial;
    uint32_t numMaterials;
    float lodThreshold{0.005f};
    float zNear;
    float4x4 view;
    float4x4 proj;
    float4 projPlanes; // ij:left, kl:top
    uint32_t hizLevels;
    uint32_t hizWidth;
    uint32_t hizHeight;
    // -- Lighting
    float3 camDirection;
    float3 sunDirection{0, 1, 0};
    float3 ambientColor{0.1, 0.1, 0.1};
};
#pragma pack(pop)

static const int kViewOverdraw = 1 << 0;
static const int kViewMeshlet = 1 << 1;
static const int kViewHIZ = 1 << 2;

static const int kCullFrustum = 1 << 0;
static const int kCullOcclusion = 1 << 1;
static const int kCullBackface = 1 << 2;

static const int kCullStageFirst = 1 << 16;
static const int kCullStageLate = 1 << 17;

static const int kGBufferViewBaseColor = 1 << 0;
static const int kGBufferViewNormal = 1 << 1;
static const int kGBufferViewMaterialID = 1 << 2;

struct RendererConfig
{
    unsigned viewFlags{};
    unsigned cullFlags{kCullFrustum | kCullOcclusion | kCullBackface};
    unsigned gbufferFlags{};
};
extern void RendererSetupImGuiOnly(FContext* context);
extern void RendererSetup(FContext* context, UBO* pShaderGlobals, RendererConfig cfg = {});
