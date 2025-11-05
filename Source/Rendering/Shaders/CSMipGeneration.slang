Texture2D srcTexture;
// ^^ Chained from previous mip level if mip >= 1
RWTexture2D dstTexture;
SamplerState sampler;

struct PushConstant {
    float2 srcExtent; // Power of 2
}
[[vk::push_constant]] PushConstant pc;

// Dispatched at half res per pass
[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint2 tid: SV_DispatchThreadID) {
    float4 color = srcTexture.Sample(sampler, (tid + float2(0.5)) / pc.srcExtent);
    dstTexture[tid] = color;
}
