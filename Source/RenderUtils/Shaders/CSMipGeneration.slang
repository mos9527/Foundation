Texture2D srcTexture;
// ^^ Chained from previous mip level if mip >= 1
RWTexture2D dstTexture;
SamplerState sampler;

[[vk::push_constant]] float2 extent;

// Dispatched at half res per pass
[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint2 tid: SV_DispatchThreadID) {
    float4 color = srcTexture.SampleGrad(sampler, (tid + float2(0.5)) / extent, float2(0), float2(0));
    dstTexture[tid] = color;
}
