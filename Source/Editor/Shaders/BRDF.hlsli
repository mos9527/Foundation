#include "Common.hlsli"
#ifndef BRDF
#define BRDF
// Heavily referenced from glTF-Sample-Viewer
// ** https://github.com/KhronosGroup/glTF-Sample-Viewer/blob/main/source/Renderer/shaders/brdf.glsl
// ** https://jcgt.org/published/0003/02/03/paper.pdf
// All VdotVs are clamped to [0,1]
// ** p319,p320
float3 F_Schlick(float3 f0, float3 f90, float VdotH)
{
    return f0 + (f90 - f0) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
}

// Isotropic implementations
// ** p340
float D_GGX(float NoH, float roughness)
{
    NoH = saturate(NoH);
    float ag2 = roughness * roughness;
    ag2 = clamp(ag2, EPSILON, 1.0f);
    float denom = M_PI * pow(1 + NoH * NoH * (ag2 - 1), 2);
    return NoH * ag2 * rcp(max(denom, EPSILON));

}

// ** p341,p337
float Vis_GGX_SmithCorrelated(float NoV, float NoL, float roughness)
{        
    float ag2 = roughness * roughness;
    ag2 = clamp(ag2, EPSILON, 1.0f);
    float uo = saturate(NoV);
    float ui = saturate(NoL);
    float GGXV = uo * sqrt(ui * ui * (1.0 - ag2) + ag2); // u0 = n*l+
    float GGXL = ui * sqrt(uo * uo * (1.0 - ag2) + ag2); // ui = n*v+
    return 0.5 * rcp(max(EPSILON, GGXV + GGXL));
}
// ** https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_sheen.pdf
float D_Charlie(float roughness, float NoH)
{
    // Estevez and Kulla 2017, "Production Friendly Microfacet Sheen BRDF"
    roughness = max(roughness, 0.000001); //clamp (0,1]
    float alphaG = roughness * roughness;
    float invAlpha = 1.0 / alphaG;
    float cos2h = NoH * NoH;
    float sin2h = 1.0 - cos2h;
    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / (2.0 * M_PI);
}
// [Estevez and Kulla 2017, "Production Friendly Microfacet Sheen BRDF"]
// ** https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Shaders/Private/BRDF.ush
// ** https://github.com/KhronosGroup/glTF/blob/main/extensions/2.0/Khronos/KHR_materials_sheen/README.md#sheen-visibility
float Vis_Charlie_L(float x, float r)
{
    r = saturate(r);
    r = 1.0 - (1. - r) * (1. - r);

    float a = lerp(25.3245, 21.5473, r);
    float b = lerp(3.32435, 3.82987, r);
    float c = lerp(0.16801, 0.19823, r);
    float d = lerp(-1.27393, -1.97760, r);
    float e = lerp(-4.85967, -4.32054, r);

    return a * rcp((1 + b * pow(x, c)) + d * x + e);
}
// ** https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_sheen.pdf
// ** https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Shaders/Private/BRDF.ush
float Vis_Charlie(float NoL, float NoV, float a)
{
    float VisV = NoV < 0.5 ? exp(Vis_Charlie_L(NoV, a)) : exp(2 * Vis_Charlie_L(0.5, a) - Vis_Charlie_L(1 - NoV, a));
    float VisL = NoL < 0.5 ? exp(Vis_Charlie_L(NoL, a)) : exp(2 * Vis_Charlie_L(0.5, a) - Vis_Charlie_L(1 - NoL, a));

    return saturate(rcp(((1 + VisV + VisL) * (4 * NoV * NoL))));
}
// ** https://github.com/EpicGames/UnrealEngine/blob/release/Engine/Shaders/Private/BRDF.ush
float Vis_Ashikhmin(float NoV, float NoL)
{
    return rcp(4 * (NoL + NoV - NoL * NoV));
}
// ** https://github.com/google/filament/blob/main/shaders/src/brdf.fs
float Vis_Neubelt(float NoV, float NoL)
{
    // Neubelt and Pettineo 2013, "Crafting a Next-gen Material Pipeline for The Order: 1886"
    return min(1.0 / (4.0 * (NoL + NoV - NoL * NoV)), 65504.0);
}

// ** p314, p351
// ** https://seblagarde.wordpress.com/2012/01/08/pi-or-not-to-pi-in-game-lighting-equation/
// ** https://google.github.io/filament/Filament.md.html#figure_lambert_vs_disney
float3 BRDF_Lambertian(float3 diffuse)
{
    return diffuse / M_PI;
}
// ** p337
float3 BRDF_GGX_Specular(
    float3 F0, float3 F90, float VdotH,
    float NdotL, float NdotV, float NdotH,
    float a
)
{
    float3 F = F_Schlick(F0, F90, VdotH);
    float Vis = Vis_GGX_SmithCorrelated(NdotL, NdotV, a);
    float D = D_GGX(NdotH, a);

    return F * Vis * D;
}

// Anisotropic implementations
// ** p345
// ** https://google.github.io/filament/Filament.md.html#listing_anisotropicbrdf
float D_GGX_Anisotropic(float TdotH, float BdotH, float NdotH, float ax, float ay)
{
    float a2 = ax * ay;
    float3 f = float3(ay * TdotH, ax * BdotH, a2 * NdotH);
    float w2 = a2 / dot(f, f);
    return a2 * w2 * w2 / M_PI;
}

// ** https://google.github.io/filament/Filament.md.html#listing_kelemen
float Vis_Kelemen(float LdotH)
{
    return 0.25 / (LdotH * LdotH);
}

// ** p344
// ** https://google.github.io/filament/Filament.md.html#listing_anisotropicv
float Vis_GGX_Combined_Anisotropic(float TdotL, float BdotL, float NdotL, float TdotV, float BdotV, float NdotV, float ax, float ay)
{
    float GGXV = NdotL * length(float3(ax * TdotV, ay * BdotV, NdotV));
    float GGXL = NdotV * length(float3(ax * TdotL, ay * BdotL, NdotL));
    float denominator = GGXV + GGXL;
    if (denominator > 0)
        return clamp(0.5 / denominator, 0, 1);
    else
        return 0; // prevent div-by-0
}
// ** p345
// ** https://blog.selfshadow.com/publications/s2012-shading-course/burley/s2012_pbs_disney_brdf_notes_v3.pdf
float3 BRDF_GGX_SpecularAnisotropic(
    float3 F0, float3 F90, float VdotH,
    float TdotL, float BdotL, float NdotL,
    float TdotH, float BdotH, float NdotH,
    float TdotV, float BdotV, float NdotV,
    float ax, float ay, float specularWeight // glTF-Sample-Viewer omitted this somehow?
)
{
    ay = clamp(ay, 0.00001, 1.0); // ** https://github.com/KhronosGroup/glTF-Sample-Viewer/blob/main/source/Renderer/shaders/brdf.glsl
    // clamp (0,1]
    // For numeric stability? Otherwise we'll get NaNs with ax=ay=0
    float D = D_GGX_Anisotropic(TdotH, BdotH, NdotH, ax, ay);
    float Vis = Vis_GGX_Combined_Anisotropic(TdotL, BdotL, NdotL, TdotV, BdotV, NdotV, ax, ay);

    float3 F = F_Schlick(F0, F90, VdotH);
    return F * Vis * D * specularWeight;
}
// ** https://google.github.io/filament/Filament.md.html#listing_clearcoatbrdf
float3 BRDF_Clearcoat(float3 F0, float3 F90, float NdotH, float LdotH, float a)
{
    float Dc = D_GGX(NdotH, a);
    float Vc = Vis_Kelemen(LdotH);
    float3 Fc = F_Schlick(F0, F90, LdotH);
    return Dc * Vc * Fc;
}
// ** https://github.com/KhronosGroup/glTF-Sample-Viewer/blob/main/source/Renderer/shaders/brdf.glsl
float3 BRDF_specularSheen(float3 sheenColor, float sheenRoughness, float NdotL, float NdotV, float NdotH)
{
    float sheenDistribution = D_Charlie(sheenRoughness, NdotH);
    float sheenVisibility = Vis_Charlie(NdotL, NdotV, sheenRoughness);
    return sheenColor * sheenDistribution * sheenVisibility;
}
// ** http://www.jp.square-enix.com/tech/library/pdf/Error%20Reduction%20and%20Simplification%20for%20Shading%20Anti-Aliasing.pdf
// ** http://www.jp.square-enix.com/tech/library/pdf/ImprovedGeometricSpecularAA(slides).pdf slide #50
float filterRoughness(float3 H, float roughness, float sigma, float kappa)
{ // Tokuyoshi17
    float2 deltaU = ddx(H.xy);
    float2 deltaV = ddy(H.xy);
    float variance = sigma * (dot(deltaU, deltaU) + dot(deltaV, deltaV));
    float kernelSquaredRoughness = min(2 * variance, kappa);
    return saturate(roughness + kernelSquaredRoughness);
}
#endif