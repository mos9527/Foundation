#ifndef LIGHTING_COMMON
#include "Shading.hlsli"
#endif
#include "BRDF.hlsli"
#define DIELECTRIC_SPECULAR 0.04
/* LTC area light sources */
void shade_ltc_quad(SceneLight light, float4 t1, float4 t2, float3x3 Minv, float3 F0, float3 c_diffuse, float3 P, float3 V, float3 N, inout float3 diffuse, inout float3 specular)
{
    float3 b1, b2;
    buildOrthonormalBasis(light.direction.xyz, b1, b2);
    
    float3 points[4];
    points[0] = light.position.xyz - b1 * light.area_quad_disk_extents.x - b2 * light.area_quad_disk_extents.y;
    points[1] = light.position.xyz + b1 * light.area_quad_disk_extents.x - b2 * light.area_quad_disk_extents.y;
    points[2] = light.position.xyz + b1 * light.area_quad_disk_extents.x + b2 * light.area_quad_disk_extents.y;
    points[3] = light.position.xyz - b1 * light.area_quad_disk_extents.x + b2 * light.area_quad_disk_extents.y;
    
    diffuse += c_diffuse * LTC_EvaluateQuad(N, V, P, float3x3(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)), points, light.area_quad_disk_twoSided) * light.intensity * light.color.rgb;
    specular += (F0 * t2.x + (splat3(1) - F0) * t2.y) * LTC_EvaluateQuad(N, V, P, Minv, points, light.area_quad_disk_twoSided) * light.intensity * light.color.rgb;
}
void shade_ltc_line(SceneLight light, float4 t1, float4 t2, float3x3 Minv, float3 F0, float3 c_diffuse, float3 P, float3 V, float3 N, inout float3 diffuse, inout float3 specular)
{
    float3 b1, b2;
    buildOrthonormalBasis(light.direction.xyz, b1, b2);
    
    float3 P1 = light.position.xyz + b2 * light.area_line_length / -2;
    float3 P2 = light.position.xyz + b2 * light.area_line_length / 2;
    float R = light.area_line_radius;
    uint endCaps = light.area_line_caps;
    diffuse += c_diffuse * LTC_EvaluateLine(N, V, P, float3x3(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)), P1, P2, R, endCaps) * light.intensity * light.color.rgb;
    specular += (F0 * t2.x + (splat3(1) - F0) * t2.y) * LTC_EvaluateLine(N, V, P, Minv, P1, P2, R, endCaps) * light.intensity * light.color.rgb;
}
void shade_ltc_disk(SceneLight light, float4 t1, float4 t2, float3x3 Minv, float3 F0, float3 c_diffuse, float3 P, float3 V, float3 N, inout float3 diffuse, inout float3 specular)
{
    float3 b1, b2;
    buildOrthonormalBasis(light.direction.xyz, b1, b2);
    
    float3 points[4];
    points[0] = light.position.xyz - b1 * light.area_quad_disk_extents.x - b2 * light.area_quad_disk_extents.y;
    points[1] = light.position.xyz + b1 * light.area_quad_disk_extents.x - b2 * light.area_quad_disk_extents.y;
    points[2] = light.position.xyz + b1 * light.area_quad_disk_extents.x + b2 * light.area_quad_disk_extents.y;
    points[3] = light.position.xyz - b1 * light.area_quad_disk_extents.x + b2 * light.area_quad_disk_extents.y;
    
    diffuse += c_diffuse * LTC_EvaluateDisk(N, V, P, float3x3(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)), points, light.area_quad_disk_twoSided) * light.intensity * light.color.rgb;
    specular += (F0 * t2.x + (splat3(1) - F0) * t2.y) * LTC_EvaluateDisk(N, V, P, Minv, points, light.area_quad_disk_twoSided) * light.intensity * light.color.rgb;
}
void shade_ltc(SceneLight light, float3 baseColor, float metal, float roughness, float3 P, float3 V, float3 N, inout float3 diffuse, inout float3 specular)
{
    float NoV = saturate(dot(N, V));
     // use roughness and sqrt(1-cos_theta) to sample M_texture
    float2 uv = float2(roughness, sqrt(1.0f - NoV));
    uv = uv * LTC_LUT_SCALE + LTC_LUT_BIAS;
    // get 4 parameters for inverse_M
    float4 t1 = LTC_SampleLut(uv, 0);
    // Get 2 parameters for Fresnel calculation
    float4 t2 = LTC_SampleLut(uv, 1);
    float3x3 Minv = float3x3(
        float3(t1.x, 0, t1.y),
        float3(0, 1, 0),
        float3(t1.z, 0, t1.w)
    );
    // metal influenced specular & diffuse
    float3 F0 = lerp(0.5f * splat3(DIELECTRIC_SPECULAR), baseColor, metal);
    float3 c_diffuse = lerp(baseColor, splat3(0), metal);
    
    switch (light.type)
    {
        case SCENE_LIGHT_TYPE_AREA_QUAD:
            shade_ltc_quad(light, t1, t2, Minv, F0, c_diffuse, P, V, N, diffuse, specular);
            break;
        case SCENE_LIGHT_TYPE_AREA_LINE:
            shade_ltc_line(light, t1, t2, Minv, F0, c_diffuse, P, V, N, diffuse, specular);
            break;
        case SCENE_LIGHT_TYPE_AREA_DISK:
            shade_ltc_disk(light, t1, t2, Minv, F0, c_diffuse, P, V, N, diffuse, specular);
            break;
    }
}
/* Infinitesimal light sources */
void attenuate_directional_light(SceneLight light, out float3 L, out float3 attenuation)
{
    L = normalize(-light.direction.xyz);
    attenuation = light.color.rgb * light.intensity;
}
void attenuate_point_light(SceneLight light, float3 P, out float3 L, out float3 attenuation)
{
    float3 l = light.position.xyz - P;
    L = normalize(l);
    float distance = length(l);
    float radius = light.spot_point_radius;
    attenuation = max(min(1.0 - pow(distance / radius, 4.0), 1.0), 0.0) / pow(distance, 2.0) * light.color.rgb * light.intensity;
}
void attenuate_spot_light(SceneLight light, float3 P, out float3 L, out float3 attenuation)
{
    float3 l = light.position.xyz - P;
    L = normalize(l);
    float distance = length(l);
    float radius = light.spot_point_radius;
    float cosAngle = dot(normalize(light.direction.xyz), -L);
    float innerCos = cos(light.spot_size_rad * (1 - light.spot_size_blend));
    float outerCos = cos(light.spot_size_rad);
    float angAtt = saturate((cosAngle - outerCos) / (innerCos - outerCos));
    attenuation = angAtt * max(min(1.0 - pow(distance / radius, 4.0), 1.0), 0.0) / pow(distance, 2.0) * light.color.rgb * light.intensity;
}
void shade_direct_brdf(float3 L, float3 V, float3 N, float3 baseColor, float metal, float alphaRoughness, float3 attenuation, inout float3 diffuse, inout float3 specular)
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

/* SHADER ENTRY POINT */
void shade_direct(SceneLight light, float3 P, float3 V, float3 N, float3 baseColor, float metal, float alphaRoughness, inout float3 diffuse, inout float3 specular)
{
    float3 L = splat3(0);
    float3 attenuation = splat3(1.0f);
    alphaRoughness = clamp(alphaRoughness, 0.001f, 0.999f);
    switch (light.type)
    {
        case SCENE_LIGHT_TYPE_POINT:
            attenuate_point_light(light, P, L, attenuation);
            shade_direct_brdf(L, V, N, baseColor, metal, alphaRoughness, attenuation, diffuse, specular);
            break;
        case SCENE_LIGHT_TYPE_DIRECTIONAL:
            attenuate_directional_light(light, L, attenuation);
            shade_direct_brdf(L, V, N, baseColor, metal, alphaRoughness, attenuation, diffuse, specular);
            break;
        case SCENE_LIGHT_TYPE_SPOT:
            attenuate_spot_light(light, P, L, attenuation);
            shade_direct_brdf(L, V, N, baseColor, metal, alphaRoughness, attenuation, diffuse, specular);
            break;
        case SCENE_LIGHT_TYPE_AREA_QUAD:
        case SCENE_LIGHT_TYPE_AREA_LINE:
        case SCENE_LIGHT_TYPE_AREA_DISK:
            shade_ltc(light, baseColor, metal, alphaRoughness, P, V, N, diffuse, specular);
            break;
    }
}
void shade_indirect(SceneIBLProbe probe, float3 V, float3 N, float3 baseColor, float metal, float perceptualRoughness, float occlusion, inout float3 diffuse, inout float3 specular)
{
    float3 F0 = lerp(0.5f * splat3(DIELECTRIC_SPECULAR), baseColor, metal);
    float3 c_diffuse = lerp(baseColor, splat3(0), metal);
    diffuse += getIBLRadianceLambertian(probe, N, V, perceptualRoughness, c_diffuse, F0, 1.0f) * lerp(1, occlusion, probe.occlusionStrength);
    specular += getIBLRadianceGGX(probe, N, V, perceptualRoughness, F0, 1.0f) * lerp(1, occlusion, probe.occlusionStrength);
}