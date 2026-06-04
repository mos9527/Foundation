#include "IFlags.slang"
#include "ICommon.slang"

Texture2D<uint32_t> instanceIDBuffer; // Instance ID map (R32_UINT), ~0u = no object
RWStructuredBuffer<uint32_t> pickResult; // [0] written when fragCoord == pickPixel
Texture2D<float4> displayImage;
uniform PostprocessUBO globalParams;

struct BlitPC { 
    int px; 
    int py; 
};
[[vk::push_constant]] BlitPC blitPC;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(int(fragCoord.x), int(fragCoord.y), 0);
    uint32_t instanceID = instanceIDBuffer.Load(coord);
    // GPU picking: if this pixel is the clicked one, write instanceID to result buffer
    if (blitPC.px >= 0 && int(fragCoord.x) == blitPC.px && int(fragCoord.y) == blitPC.py)
        pickResult[0] = instanceID;

    float4 result = displayImage.Load(coord);
    // Cross filter for a simple outline
    uint32_t pickID = globalParams.outlineInstanceId;
    if (globalParams.postShowOutline != 0 && pickID != ~0u) {
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