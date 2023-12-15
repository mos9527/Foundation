#ifndef LIGHTING_COMMON
#define LIGHTING_COMMON
#include "Shared.h"
/* LTC Sampler */
#ifndef NO_LTC
float4 LTC_SampleLut(float2 uv, uint index)
{
    Texture2DArray<float4> ltcLUT = ResourceDescriptorHeap[g_Shader.ltcLut];
    return ltcLUT.SampleLevel(g_TextureSampler, float3(uv, index), 0);
}
#include "LTC.hlsli"
#endif

#ifndef NO_IBL
/* IBL Sampler */
float4 getDiffuseLight(SceneIBLProbe probe, float3 n)
{
    TextureCube cube = ResourceDescriptorHeap[probe.irradianceHeapIndex];
    return cube.SampleLevel(g_TextureSampler, n, 0) * probe.diffuseIntensity;
}

float4 getSpecularSample(SceneIBLProbe probe, float3 reflection, float lod)
{
    TextureCubeArray cubes = ResourceDescriptorHeap[probe.radianceHeapIndex];
    return cubes.SampleLevel(g_TextureSampler, float4(reflection, IBL_PROBE_SPECULAR_GGX), lod) * probe.specularIntensity;
}

float4 getSheenSample(SceneIBLProbe probe, float3 reflection, float lod)
{
    TextureCubeArray cubes = ResourceDescriptorHeap[probe.radianceHeapIndex];
    return cubes.SampleLevel(g_TextureSampler, float4(reflection, IBL_PROBE_SPECULAR_SHEEN), lod) * probe.specularIntensity;
}

float4 getLUT(SceneIBLProbe probe, float NoV, float roughness)
{
    return float4(0, 0, 0, 0); // xxx
}
#define IBL_SHADING
#include "IBL.hlsli"
#endif

#ifndef NO_SHADING
// SHADING ENTRY POINT
#include "ShadingPBR.hlsli"
#endif
#endif
