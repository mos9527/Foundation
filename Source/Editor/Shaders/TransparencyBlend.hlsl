#include "Common.hlsli"
#define SHADER_CONSTANT_32_8_TYPE TransparencyBlendConstant
#include "Bindless.hlsli"
[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > uint2(g_Shader32.width, g_Shader32.height)))
        return;
    Texture2D<float4> accTex = ResourceDescriptorHeap[g_Shader32.accumalationSrvHandle];
    Texture2D<float4> revTex = ResourceDescriptorHeap[g_Shader32.revealageSrvHandle];
    RWTexture2D<float4> framebuffer = ResourceDescriptorHeap[g_Shader32.framebufferUavHandle];
    float3 C0 = framebuffer[DTid].rgb;
    float4 acc = accTex[DTid];
    float rev = revTex[DTid].r;
    // https://jcgt.org/published/0002/02/09/paper.pdf eq. 6
    float3 Ci_sum = acc.rgb;
    float ai_sum = clamp(acc.a, 1e-4, 5e4);
    float inv_ai_prod = rev;
    float3 Cf = (Ci_sum / ai_sum) * (1 - inv_ai_prod) + C0 * inv_ai_prod;    
    framebuffer[DTid] = float4(Cf, 1.0f);
}