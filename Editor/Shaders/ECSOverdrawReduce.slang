struct PushConstant {
    int w, h;
}
[[vk::push_constant]] PushConstant pc;

Texture2D<uint32_t> texture;
RWStructuredBuffer<Atomic<uint32_t>> globalMax;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid: SV_DispatchThreadID, uint gid : SV_GroupIndex) {
    uint32_t value = 0u;
    if (tid.x < pc.w && tid.y < pc.h)
        value = texture.Load(int3(tid.x,tid.y,0));
    uint32_t waveMax = WaveActiveMax(value);
    if (WaveIsFirstLane())
        globalMax[0].max(waveMax);
}

