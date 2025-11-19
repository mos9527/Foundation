struct MeshPayload
{
    float time;
};

[[vk::push_constant]] float time;
groupshared MeshPayload p;

[numthreads(1, 1, 1)]
[shader("amplification")]
void taskMain(in uint tig : SV_GroupIndex)
{
    p.time = time;
    DispatchMesh(64,1,1,p);
}
