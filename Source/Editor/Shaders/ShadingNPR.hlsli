#ifndef LIGHTING_COMMON
#include "Shading.hlsli"
#endif

/* SHADER ENTRY POINT */
void shade_direct(SceneLight light, float3 P, float3 V, float3 N, float3 baseColor, float metal, float alphaRoughness, inout float3 diffuse, inout float3 specular)
{
    diffuse += baseColor;
}
void shade_indirect(SceneIBLProbe probe, float3 V, float3 N, float3 baseColor, float metal, float perceptualRoughness, float occlusion, inout float3 diffuse, inout float3 specular)
{

}