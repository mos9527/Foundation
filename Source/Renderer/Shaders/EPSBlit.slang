#include "IFlags.slang"
#include "ICommon.slang"

Texture2D<uint32_t> instanceIDBuffer; // Instance ID map (R32_UINT), ~0u = no object
RWStructuredBuffer<uint32_t> pickResult; // [0] written when fragCoord == pickPixel
Texture2D<float4> displayImage;
SamplerState displaySampler;
uniform PostprocessUBO globalParams;

struct BlitPC { 
    int px; 
    int py; 
};
[[vk::push_constant]] BlitPC blitPC;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    // UV-based sample for the display image (handles resolution mismatch / upscale)
    float4 result = displayImage.Sample(displaySampler, uv);

    // Map display-space UV to internal render resolution for integer-addressed buffers
    float2 internalCoord = uv * float2(globalParams.renderWidth, globalParams.renderHeight);
    int3 coord = int3(int(internalCoord.x), int(internalCoord.y), 0);
    uint32_t instanceID = instanceIDBuffer.Load(coord);

    // GPU picking: push constant pick pixel is in display space, scale to internal resolution
    if (blitPC.px >= 0) {
        float2 pickUV = (float2(blitPC.px, blitPC.py) + 0.5f) / float2(globalParams.fbWidth, globalParams.fbHeight);
        int2 internalPick = int2(pickUV * float2(globalParams.renderWidth, globalParams.renderHeight));
        if (int(internalCoord.x) == internalPick.x && int(internalCoord.y) == internalPick.y)
            pickResult[0] = instanceID;
    }

    // Cross filter for a simple outline
    uint32_t pickID = globalParams.outlineInstanceId;
    if (globalParams.dbgShowOutline != 0 && pickID != ~0u) {
        bool isSel  = (instanceID == pickID);
        bool selL = (instanceIDBuffer.Load(coord + int3(-1, 0, 0)) == pickID);
        bool selR = (instanceIDBuffer.Load(coord + int3( 1, 0, 0)) == pickID);
        bool selT = (instanceIDBuffer.Load(coord + int3( 0,-1, 0)) == pickID);
        bool selB = (instanceIDBuffer.Load(coord + int3( 0, 1, 0)) == pickID);
        bool isEdge = isSel && !(selL && selR && selT && selB);
        if (isEdge)
            result = float4(1.0, 0.6, 0.1, 1.0);
    }
    return result;
}