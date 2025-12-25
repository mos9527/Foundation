#include "IFlags.slang"
[[SpecializationConstant]] int flags;
Texture2D lighting;
StructuredBuffer<float> sceneLuma;


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

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(fragCoord.x, fragCoord.y, 0);
    if (flags & (kViewBaseColor | kViewNormal | kViewMaterialID | kViewPosition)) {
        // Do not post process these
        return lighting.Load(coord);
    } else {
        // Main postprocess routine
        float Lavg = sceneLuma.Load(0u);
        float3 Lpix = lighting.Load(coord).xyz;
        // Exposure
        float3 L = Lpix/(Lavg * 9.6f);
        // ACES
        L = ACESFilm(L);
        // Inverse Gamma EOTF
        L = pow(L, 1.0f/2.2f);
        return float4(L, 1.0f);
    }
}
