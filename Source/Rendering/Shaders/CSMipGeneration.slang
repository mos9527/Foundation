Texture2D srcTexture;
// ^^ Chained from previous mip level
RWTexture2D dstTexture;

struct PushConstant {
    uint2 srcExtent; // Power of 2
    uint filter;    // 0 = box, 1 = min, 2 = max
}
[[vk::push_constant]] PushConstant pc;

// Dispatched at half res per pass
[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint2 tid: SV_DispatchThreadID, uint2 gid: SV_GroupID, uint2 gtid: SV_GroupThreadID) {
    float4 a = srcTexture.Load(int3(tid * 2 + uint2(0, 0), 0));
    float4 b = srcTexture.Load(int3(tid * 2 + uint2(1, 0), 0));
    float4 c = srcTexture.Load(int3(tid * 2 + uint2(0, 1), 0));
    float4 d = srcTexture.Load(int3(tid * 2 + uint2(1, 1), 0));
    float4 color;
    switch (pc.filter){
        case 1: // min
            color = min(min(a, b), min(c, d));
            break;
        case 2: // max
            color = max(max(a, b), max(c, d));
            break;
        default: // box
            color = (a + b + c + d) * 0.25;
            break;
    }
    dstTexture[tid] = color;
}