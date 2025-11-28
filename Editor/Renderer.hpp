#pragma once
#include "Scene.hpp"
#pragma pack(push, 1)
struct UBO
{
    uint32_t firstInstance;
    uint32_t numInstances;
    float lodThreshold{0.01f};
    float zNear;
    float4x4 view;
    float4x4 proj;
};
#pragma pack(pop)

extern void RendererSetupImGuiOnly(FContext* context);
extern void RendererSetup(FContext* context, UBO const* pShaderGlobals);