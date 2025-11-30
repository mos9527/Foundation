Texture2D srcTexture;
// ^^ Chained from previous mip level if mip >= 1
RWTexture2D dstTexture;
SamplerState sampler;

[[vk::push_constant]] float2 extent;

// Dispatched at half res per pass
[shader("compute")]
[numthreads(32, 32, 1)]
void csMain(uint2 tid: SV_DispatchThreadID) {
    float4 color = srcTexture.SampleLevel(sampler, (tid + float2(0.5)) / extent, 0.0);
    dstTexture[tid] = color;
}
