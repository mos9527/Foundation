#ifndef LIGHTING_COMMON
#define LIGHTING_COMMON
#include "BRDF.hlsli"
#include "Shared.h"
/* IBL */
// Ported from https://github.com/KhronosGroup/glTF-Sample-Viewer/blob/main/source/Renderer/shaders/ibl.glsl
float4 getDiffuseLight(SceneIBLProbe probe, float3 n)
{
    TextureCube cube = ResourceDescriptorHeap[probe.irradianceHeapIndex];
    return cube.SampleLevel(g_Sampler, n, 0) * probe.diffuseIntensity;
}

float4 getSpecularSample(SceneIBLProbe probe, float3 reflection, float lod)
{
    TextureCubeArray cubes = ResourceDescriptorHeap[probe.radianceHeapIndex];
    return cubes.SampleLevel(g_Sampler, float4(reflection, IBL_PROBE_SPECULAR_GGX), lod) * probe.specularIntensity;    
}

float4 getSheenSample(SceneIBLProbe probe, float3 reflection, float lod)
{
    TextureCubeArray cubes = ResourceDescriptorHeap[probe.radianceHeapIndex];
    return cubes.SampleLevel(g_Sampler, float4(reflection, IBL_PROBE_SPECULAR_SHEEN), lod) * probe.specularIntensity;
}

float4 getLUT(SceneIBLProbe probe, float NoV, float roughness)
{
    Texture2D lut = ResourceDescriptorHeap[probe.lutHeapIndex];
    return lut.SampleLevel(g_Sampler, float2(NoV, roughness), 0);
}

float3 getIBLRadianceGGX(SceneIBLProbe probe, float3 n, float3 v, float roughness, float3 F0, float specularWeight)
{
    float NdotV = saturate(dot(n, v));
    float lod = roughness * float(probe.mips - 1);
    float3 reflection = normalize(reflect(-v, n));
    
    float2 f_ab = getLUT(probe, saturate(NdotV), saturate(roughness)).rg;
    float4 specularSample = getSpecularSample(probe, reflection, lod);

    float3 specularLight = specularSample.rgb;

    // see https://bruop.github.io/ibl/#single_scattering_results at Single Scattering Results
    // Roughness dependent fresnel, from Fdez-Aguera
    float3 Fr = max(splat3(1.0 - roughness), F0) - F0;
    float3 k_S = F0 + Fr * pow(1.0 - NdotV, 5.0);
    float3 FssEss = k_S * f_ab.x + f_ab.y;

    return specularWeight * specularLight * FssEss;
}

float3 getIBLRadianceLambertian(SceneIBLProbe probe, float3 n, float3 v, float roughness, float3 diffuseColor, float3 F0, float specularWeight)
{
    float NdotV = saturate(dot(n, v));
    float2 f_ab = getLUT(probe, saturate(NdotV), saturate(roughness)).rg;

    float3 irradiance = getDiffuseLight(probe,n);

    // see https://bruop.github.io/ibl/#single_scattering_results at Single Scattering Results
    // Roughness dependent fresnel, from Fdez-Aguera

    float3 Fr = max(splat3(1.0 - roughness), F0) - F0;
    float3 k_S = F0 + Fr * pow(1.0 - NdotV, 5.0);
    float3 FssEss = specularWeight * k_S * f_ab.x + f_ab.y; // <--- GGX / specular light contribution (scale it down if the specularWeight is low)

    // Multiple scattering, from Fdez-Aguera
    float Ems = (1.0 - (f_ab.x + f_ab.y));
    float3 F_avg = specularWeight * (F0 + (1.0 - F0) / 21.0);
    float3 FmsEms = Ems * FssEss * F_avg / (1.0 - F_avg * Ems);
    float3 k_D = diffuseColor * (1.0 - FssEss + FmsEms); // we use +FmsEms as indicated by the formula in the blog post (might be a typo in the implementation)

    return (FmsEms + k_D) * irradiance;
}

/* SHADING */
void shade_direct(float3 L, float3 V, float3 N, float3 baseColor, float metal, float alphaRoughness, float3 attenuation, inout float3 diffuse, inout float3 specular)
{
    float3 H = normalize(L + V);
    
    float NoL = clampedDot(N, L);
    float VoH = clampedDot(V, H);
    float NoH = clampedDot(N, H);
    float NoV = clampedDot(N, V);
        
    float3 F0 = lerp(0.5f * splat3(DIELECTRIC_SPECULAR), baseColor, metal);
    float3 c_diffuse = lerp(baseColor, splat3(0), metal);    
    diffuse += attenuation * NoL * BRDF_Lambertian(c_diffuse);
    specular += attenuation * NoL * BRDF_GGX_Specular(F0, splat3(1), VoH, NoL, NoV, NoH, alphaRoughness);
}
void shade_indirect(SceneIBLProbe probe, float3 V, float3 N, float3 baseColor, float metal, float perceptualRoughness, float occlusion, inout float3 diffuse, inout float3 specular)
{    
    float3 F0 = lerp(0.5f * splat3(DIELECTRIC_SPECULAR), baseColor, metal);
    float3 c_diffuse = lerp(baseColor, splat3(0), metal);
    diffuse += getIBLRadianceLambertian(probe, N, V, perceptualRoughness, c_diffuse, F0, 1.0f) * lerp(1, occlusion, probe.occlusionStrength);
    specular += getIBLRadianceGGX(probe, N, V, perceptualRoughness, F0, 1.0f) * lerp(1, occlusion , probe.occlusionStrength);
}
#endif