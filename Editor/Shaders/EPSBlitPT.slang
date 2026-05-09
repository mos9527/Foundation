import IFlags;
import ICommon;
Texture2D<uint32_t> pickIDBuffer; // Instance ID map (R32_UINT), ~0u = no object
RWStructuredBuffer<uint32_t> pickResult; // [0] written when fragCoord == pickPixel
Texture2D<float4> displayImage;
uniform UBO globalParams;

struct PickPC { int px; int py; };
[[vk::push_constant]] PickPC pickPC;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    int3 coord = int3(int(fragCoord.x), int(fragCoord.y), 0);
    uint32_t instanceID = pickIDBuffer.Load(coord);
    uint32_t pickID = pickResult[0];
    // GPU picking: if this pixel is the clicked one, write instanceID to result buffer
    if (pickPC.px >= 0 && int(fragCoord.x) == pickPC.px && int(fragCoord.y) == pickPC.py)
        pickResult[0] = instanceID;

    float4 result = displayImage.Load(coord);

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