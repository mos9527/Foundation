#ifndef LIGHTING_COMMON
#define LIGHTING_COMMON
#include "BRDF.hlsli"
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
#endif