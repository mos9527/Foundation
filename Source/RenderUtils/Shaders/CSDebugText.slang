struct PushConstant {
    int x, y, w, h;
    uint32_t text[64/4];
}
[[vk::binding(0, 0)]] RWTexture2D texture;
[[vk::push_constant]] PushConstant pc;


[shader("compute")]
[numthreads(8, 8, 1)]
void debugText(uint2 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.w || tid.y >= pc.h) return;
    tid.x += pc.x, tid.y += pc.y;
    texture[tid] = float4(1,0,1,1);
}
