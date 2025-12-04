#pragma once
#include "Scene.hpp"
#pragma pack(push, 1)
struct UBO
{
    uint32_t firstInstance;
    uint32_t numInstances;
    float lodThreshold{0.005f};
    float zNear;
    float4x4 view;
    float4x4 proj;
    float4 projPlanes; // ij:left, kl:top
    uint32_t hizLevels;
    uint32_t hizWidth;
    uint32_t hizHeight;
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

struct RendererConfig
{
    unsigned viewFlags{kViewMeshlet};
    unsigned cullFlags{kCullFrustum|kCullOcclusion|kCullBackface};
};
extern void RendererSetupImGuiOnly(FContext* context);
extern void RendererSetup(FContext* context, UBO* pShaderGlobals, RendererConfig cfg = {});