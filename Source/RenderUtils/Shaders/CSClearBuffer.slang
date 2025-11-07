struct PushConstant {
    float4 color;
    int w, h;
}
[[vk::binding(0, 0)]] RWTexture2D texture;
[[vk::push_constant]] PushConstant pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void debugText(uint2 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.w || tid.y >= pc.h) return;
    texture[tid] = pc.color;
}
