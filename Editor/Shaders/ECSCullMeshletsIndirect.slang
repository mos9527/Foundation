#include "ICommon.slang"

RWStructuredBuffer<uint> counter;
RWStructuredBuffer<MeshletTaskDispatch> dispatch;


[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint tid: SV_DispatchThreadID) {
    uint N = counter[0];
    uint mod = N % kMeshIndirectPerDrawMeshlets;
    dispatch[N / kMeshIndirectPerDrawMeshlets].groupCountX = mod ? mod : kMeshIndirectPerDrawMeshlets;
}
