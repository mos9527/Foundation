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
};
#pragma pack(pop)
BITMASK_ENUM_BEGIN(RendererSetupFlags, uint32_t)
    DebugViewOverdraw = 1u << 0,
    DebugViewMeshlet = 1u << 1
BITMASK_ENUM_END()
extern void RendererSetupImGuiOnly(FContext* context);
extern void RendererSetup(FContext* context, UBO const* pShaderGlobals, RendererSetupFlags flags = {});