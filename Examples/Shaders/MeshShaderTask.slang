struct MeshPayload
{
    int exponent;
};

groupshared MeshPayload p;

[numthreads(1, 1, 1)]
[shader("amplification")]
void taskMain(in uint tig : SV_GroupIndex)
{
    p.exponent = 3;
    DispatchMesh(1,1,1,p);
}
