#pragma once
#include "Scene.hpp"
#pragma pack(push, 1)
struct UBO
{
    uint32_t firstInstance;
    uint32_t numInstances;
    float lodThreshold{0.25f};
    float zNear;
    float4x4 view;
    float4x4 proj;
    float4 projPlanes; // ij:left, kl:top
};
#pragma pack(pop)

static const int kViewOverdraw = 1 << 0;
static const int kViewMeshlet = 1 << 1;

static const int kCullFrustum = 1 << 0;
static const int kCullOcclusion = 1 << 1;
static const int kCullBackface = 1 << 2;

struct RendererSetupFlags
{
    int viewFlags{0};
    int cullFlags{0};
};
extern void RendererSetupImGuiOnly(FContext* context);
extern void RendererSetup(FContext* context, UBO const* pShaderGlobals, RendererSetupFlags flags = {});