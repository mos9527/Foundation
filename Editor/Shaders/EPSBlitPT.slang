#include "IFlags.slang"
#include "ICommon.slang"
import glsl;
[[SpecializationConstant]] int flags;
// Raw accumulation buffers (F32 sums — averaged here)
Texture2D accumDiffuseTex;
Texture2D accumSpecularTex;
Texture2D accumGBuffer0Tex;  // BaseColor.rgb, MaterialID (packed)
Texture2D accumGBuffer1Tex;  // Normal.xyz * 0.5 + 0.5, 0
Texture2D accumGBuffer2Tex;  // Position.xyz, 0
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
    float3 L = hdr * exp2(-globalParams.camEV);
    L = ACESFilm(L);
    return pow(L, 1.0f/2.2f);
}

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(fragCoord.x, fragCoord.y, 0);
    float frameCount = max(float(globalParams.ptAccumualatedFrames + 1), 1.0f);
    float rcpFrames = 1.0f / frameCount;

    // GBuffer debug views — averaged from accumulation, no tone mapping
    if (flags & kViewBaseColor) {
        float3 baseColor = accumGBuffer0Tex.Load(coord).xyz * rcpFrames;
        return float4(baseColor, 1.0f);
    }
    if (flags & kViewNormal) {
        float3 normal = accumGBuffer1Tex.Load(coord).xyz * rcpFrames;
        return float4(normal, 1.0f);
    }
    if (flags & kViewMaterialID) {
        float matIDPacked = accumGBuffer0Tex.Load(coord).w * rcpFrames;
        uint materialID = uint(matIDPacked * 255.0f + 0.5f);
        return float4(unpackUnorm4x8ToFloat(hash(materialID)).xyz, 1.0f);
    }
    if (flags & kViewPosition) {
        float3 position = accumGBuffer2Tex.Load(coord).xyz * rcpFrames;
        return float4(position, 1.0f);
    }

    // Per-lobe AOV isolation views (averaged + tone mapped)
    if (flags & (kViewAOVDiffuse | kViewAOVSpecular)) {
        float3 aov = float3(0);
        if (flags & kViewAOVDiffuse)  aov = accumDiffuseTex.Load(coord).xyz * rcpFrames;
        if (flags & kViewAOVSpecular) aov = accumSpecularTex.Load(coord).xyz * rcpFrames;
        return float4(ToneMap(aov), 1.0f);
    }

    // Composite lighting = (diffuse + specular) / frameCount, then tone map
    float3 diffuse  = accumDiffuseTex.Load(coord).xyz;
    float3 specular = accumSpecularTex.Load(coord).xyz;
    float3 Lpix = (diffuse + specular) * rcpFrames;
    return float4(ToneMap(Lpix), 1.0f);
}
