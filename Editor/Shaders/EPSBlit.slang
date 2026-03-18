#include "IFlags.slang"
#include "ICommon.slang"
[[SpecializationConstant]] int flags;

StructuredBuffer<uint32_t> globalMax;
Texture2D<uint32_t> overdraw;
Texture2D lighting;
uniform UBO globalParams;

float3 heatmapGradient(float t) {
    float3 c0 = float3(0.000, 0.000, 0.002);
    float3 c1 = float3(0.250, 0.000, 0.350);
    float3 c2 = float3(0.700, 0.100, 0.200);
    float3 c3 = float3(0.950, 0.500, 0.000);
    float3 c4 = float3(1.000, 0.950, 0.600);
    float3 col = c0;
    if (t < 0.25) {
        col = lerp(c0, c1, t / 0.25);
    } else if (t < 0.5) {
        col = lerp(c1, c2, (t - 0.25) / 0.25);
    } else if (t < 0.75) {
        col = lerp(c2, c3, (t - 0.50) / 0.25);
    } else {
        col = lerp(c3, c4, (t - 0.75) / 0.25);
    }
    return col;
}

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
    if (flags & kViewOverdraw){
        uint32_t draws = overdraw.Load(coord);
        return float4(heatmapGradient((float)draws / globalMax[0]), 1.0);
    } else if (flags & kViewMeshlet){
        float4 lighting = lighting.Load(coord);
        return lighting;
    } else if (flags & (kViewBaseColor | kViewNormal | kViewMaterialID | kViewPosition)) {
        // Do not post process these
        return lighting.Load(coord);
    } else {
        // Main postprocess routine
        float3 Lpix = lighting.Load(coord).xyz;
        // Manual EV exposure
        float3 L = Lpix * exp2(-globalParams.camEV);
        // ACES
        L = ACESFilm(L);
        // Inverse Gamma EOTF
        L = pow(L, 1.0f/2.2f);
        return float4(L, 1.0f);
    }
}
