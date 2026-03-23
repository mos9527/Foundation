#include "IFlags.slang"
#include "ICommon.slang"
import glsl;
[[SpecializationConstant]] int flags;
// Buffers (Welford online mean — already averaged)
Texture2D diffuseTex;
Texture2D specularTex;
Texture2D gBuffer0Tex;  // BaseColor.rgb, MaterialID (packed)
Texture2D gBuffer1Tex;  // Normal.xyz * 0.5 + 0.5, 0
Texture2D gBuffer2Tex;  // Position.xyz, 0
uniform UBO globalParams;


// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
float3 ACESFilm(float3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}

float3 ToneMap(float3 hdr) {
    float3 L = hdr * exp2(globalParams.camEV);
    L = ACESFilm(L);
    return pow(L, 1.0f/2.2f);
}

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(fragCoord.x, fragCoord.y, 0);

    // GBuffer debug views — buffers already hold running averages (Welford)
    if (flags & kViewBaseColor) {
        float3 baseColor = gBuffer0Tex.Load(coord).xyz;
        return float4(baseColor, 1.0f);
    }
    if (flags & kViewNormal) {
        float3 normal = gBuffer1Tex.Load(coord).xyz;
        return float4(normal, 1.0f);
    }
    if (flags & kViewMaterialID) {
        float matIDPacked = gBuffer0Tex.Load(coord).w;
        uint materialID = uint(matIDPacked * 255.0f + 0.5f);
        return float4(unpackUnorm4x8ToFloat(hash(materialID)).xyz, 1.0f);
    }
    if (flags & kViewPosition) {
        float3 position = gBuffer2Tex.Load(coord).xyz;
        return float4(position, 1.0f);
    }

    // Per-lobe AOV isolation views (tone mapped)
    if (flags & (kViewAOVDiffuse | kViewAOVSpecular)) {
        float3 aov = float3(0);
        if (flags & kViewAOVDiffuse)  aov = diffuseTex.Load(coord).xyz;
        if (flags & kViewAOVSpecular) aov = specularTex.Load(coord).xyz;
        return float4(ToneMap(aov), 1.0f);
    }

    // Composite lighting = diffuse + specular (already averaged), then tone map
    float3 diffuse  = diffuseTex.Load(coord).xyz;
    float3 specular = specularTex.Load(coord).xyz;
    float3 Lpix = diffuse + specular;
    return float4(ToneMap(Lpix), 1.0f);
}
