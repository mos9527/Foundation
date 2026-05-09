#include "IFlags.slang"
#include "ICommon.slang"
#include "IPostprocess.slang"
[[SpecializationConstant]] int flags;

StructuredBuffer<uint32_t> globalMax;
Texture2D<uint32_t> overdraw;
Texture2D<uint32_t> pickIDBuffer; // Instance ID map (R32_UINT), ~0u = no object
RWStructuredBuffer<uint32_t> pickResult; // [0] written when fragCoord == pickPixel
Texture2D lighting;
// ACES OCIO-baked view LUTs. Input domain: ACEScct in [0,1].
Texture3D<float4> viewLutSdr;
Texture3D<float4> viewLutHdr;
SamplerState lutSampler;
uniform UBO globalParams;

struct BlitPC { 
    int px; 
    int py; 
};
[[vk::push_constant]] BlitPC blitPC;
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
    int3 coord = int3(fragCoord.x, fragCoord.y, 0);
    uint32_t instanceID = pickIDBuffer.Load(coord);
    uint32_t pickID = pickResult[0];
    // GPU picking: if this pixel is the clicked one, write instanceID to result buffer
    if (blitPC.px >= 0 && int(fragCoord.x) == blitPC.px && int(fragCoord.y) == blitPC.py)
        pickResult[0] = instanceID;

    PostprocessInfo pp;
    pp.Exposure = globalParams.camEV;
    pp.HdrEnabled = globalParams.enableHDR;
    pp.HdrPaperWhiteNits = globalParams.paperWhiteNits;

    float4 result;
    if (flags & kViewOverdraw){
        uint32_t draws = overdraw.Load(coord);
        result = float4(heatmapGradient((float)draws / globalMax[0]), 1.0);
    } else if (flags & (kViewBaseColor | kViewNormal | kViewMaterialID | kViewPosition | kViewMeshlet)) {
        // Do not post process these
        result = lighting.Load(coord);
    } else {
        // Main postprocess routine: exposure + BT.709->AP1 BFD + ACEScct shaper + 3D LUT
        float3 Lpix = lighting.Load(coord).xyz;
        result = float4(Postprocess(Lpix, pp, viewLutSdr, viewLutHdr, lutSampler), 1.0f);
    }

    // Cross filter for a simple outline
    if (globalParams.postShowOutline != 0 && pickID != ~0u) {
        bool isSel  = (instanceID == pickID);
        bool selL = (pickIDBuffer.Load(coord + int3(-1, 0, 0)) == pickID);
        bool selR = (pickIDBuffer.Load(coord + int3( 1, 0, 0)) == pickID);
        bool selT = (pickIDBuffer.Load(coord + int3( 0,-1, 0)) == pickID);
        bool selB = (pickIDBuffer.Load(coord + int3( 0, 1, 0)) == pickID);
        bool isEdge = isSel && !(selL && selR && selT && selB);
        if (isEdge)
            result = float4(1.0, 0.6, 0.1, 1.0);
    }
    return result;
}