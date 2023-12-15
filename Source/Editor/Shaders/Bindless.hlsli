#include "Common.hlsli"
// Before importing Bindless.hlsli...
// :: Define SHADER_CONSTANT_TYPE to specifiy per-shader constant buffer type
// :: Define SHADER_CONSTANT_32_8_TYPE to specifify per-shader [up to 8] 32 bit value type
#ifndef SHADER_CONSTANT_TYPE
#define SHADER_CONSTANT_TYPE SceneGlobals
#pragma message("Shader global constants undefined. g_Shader access will result in UB.")
#endif
#ifndef SHADER_CONSTANT_32_8_TYPE
#define SHADER_CONSTANT_32_8_TYPE SceneGlobals
#pragma message("Shader global 8*32bit constants undefined. g_Shader32 access will result in UB.")
#endif
ConstantBuffer<SceneGlobals> g_Scene : register(b0, space0);
ConstantBuffer<SHADER_CONSTANT_TYPE> g_Shader : register(b1, space0);
ConstantBuffer<SHADER_CONSTANT_32_8_TYPE> g_Shader32 : register(b2, space0);

SamplerState g_TextureSampler : register(s0, space0);
SamplerState g_DepthCompSampler : register(s1, space0);
SamplerState g_DepthReduceSampler : register(s2, space0);

// SceneGlobals helpers
uint GetSceneMeshInstanceCount()
{
    return g_Scene.meshInstances.count;
}
SceneMeshInstanceData GetSceneMeshInstance(uint index)
{
    StructuredBuffer<SceneMeshInstanceData> buffer = ResourceDescriptorHeap[g_Scene.meshInstances.heapIndex];
    return buffer.Load(index);
}
uint GetSceneMaterialCount()
{
    return g_Scene.materials.count;
}
SceneMaterial GetSceneMaterial(uint index)
{
    StructuredBuffer<SceneMaterial> buffer = ResourceDescriptorHeap[g_Scene.materials.heapIndex];
    return buffer.Load(index);
}
uint GetSceneLightCount()
{
    return g_Scene.lights.count;
}
SceneLight GetSceneLight(uint index)
{
    StructuredBuffer<SceneLight> buffer = ResourceDescriptorHeap[g_Scene.lights.heapIndex];
    return buffer.Load(index);
}