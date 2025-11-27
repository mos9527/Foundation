struct PushConstant {
    int w, h;
}
[[vk::push_constant]] PushConstant pc;

Texture2D<uint32_t> texture;
RWStructuredBuffer<Atomic<uint32_t>> globalMax;
groupshared Atomic<uint32_t> groupMax;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid: SV_DispatchThreadID, uint gid : SV_GroupIndex) {
    groupMax = 0;
    GroupMemoryBarrierWithGroupSync(); // Init
    uint32_t value = 0u;
    if (tid.x < pc.w && tid.y < pc.h)
        value = texture.Load(int3(tid.x,tid.y,0));
    uint32_t waveMax = WaveActiveMax(value);
    if (WaveIsFirstLane())
        groupMax.max(waveMax);
    GroupMemoryBarrierWithGroupSync(); // Wait for all waves
    if (gid == 0)
        globalMax[0].max(groupMax.load());
}

