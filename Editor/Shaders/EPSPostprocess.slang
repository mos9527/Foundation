#include "IFlags.slang"
#include "ICommon.slang"
#include "IPostprocess.slang"
[[SpecializationConstant]] int flags;

StructuredBuffer<uint32_t> globalMax;
Texture2D<uint32_t> overdraw;
Texture2D<float4> lighting;
// OCIO-baked view LUT. Input domain: ACEScct in [0,1].
Texture3D<float4> viewLut;
SamplerState lutSampler;
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

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(int(fragCoord.x), int(fragCoord.y), 0);

    if ((flags & kViewOverdraw) != 0) {
        uint32_t draws = overdraw.Load(coord);
        return float4(heatmapGradient((float)draws / max(globalMax[0], 1u)), 1.0);
    }

    float4 src = lighting.Load(coord);
    if ((flags & (kViewBaseColor | kViewNormal | kViewMaterialID | kViewPosition | kViewMeshlet)) != 0)
        return src;

    PostprocessInfo pp;
    pp.Exposure = globalParams.camEV;
    return float4(Postprocess(src.xyz, pp, viewLut, lutSampler), 1.0f);
}
