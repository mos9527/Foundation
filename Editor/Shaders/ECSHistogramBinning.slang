#include "ICommon.slang"

uniform UBO globalParams;

Texture2D<float4> lighting;
RWStructuredBuffer<Atomic<uint>> bins; // 64 bins. Don't forget to clear.

groupshared uint binGS[64];
[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid: SV_DispatchThreadID, uint gid : SV_GroupIndex) {
    binGS[gid] = 0;
    GroupMemoryBarrierWithGroupSync();

    float4 value = 0u;
    if (tid.x < globalParams.fbWidth && tid.y < globalParams.fbHeight)
        value = lighting.Load(int3(tid.x,tid.y,0));
    float luma = dot(value.xyz, LUMINANCE_RGB);
    float EV = log2(luma + EPS);
    int bin = clamp((int)floor(saturate((EV - globalParams.camMinEV) / (globalParams.camMaxEV - globalParams.camMinEV)) * 64.0),0, 63);
    InterlockedAdd(binGS[bin], 1);
    // Collect GS results into global
    GroupMemoryBarrierWithGroupSync();
    bins[gid].add(binGS[gid]);
}
