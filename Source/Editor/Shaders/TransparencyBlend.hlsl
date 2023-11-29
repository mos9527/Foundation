#include "Common.hlsli"
cbuffer BlendConstants : register(b0, space0)
{
    uint framebufferUavHandle;
    uint accumalationSrvHandle;
    uint revealageSrvHandle;
    uint width;
    uint height;
}
[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > uint2(width, height)))
        return;
    Texture2D<float4> accTex = ResourceDescriptorHeap[accumalationSrvHandle];
    Texture2D<float4> revTex = ResourceDescriptorHeap[revealageSrvHandle];
    RWTexture2D<float3> framebuffer = ResourceDescriptorHeap[framebufferUavHandle];
    float3 C0 = framebuffer[DTid].rgb;
    float4 acc = accTex[DTid];
    float rev = revTex[DTid].r;
    // https://jcgt.org/published/0002/02/09/paper.pdf eq. 6
    float3 Ci_sum = acc.rgb;
    float ai_sum = clamp(acc.a, 1e-4, 5e4);
    float inv_ai_prod = rev;
    float3 Cf = (Ci_sum / ai_sum) * (1 - inv_ai_prod) + C0 * inv_ai_prod;    
    framebuffer[DTid] = Cf;
}