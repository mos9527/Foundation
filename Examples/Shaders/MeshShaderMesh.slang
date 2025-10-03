// Q: Why is this seperated into *3* different files?
// A: https://github.com/KhronosGroup/SPIRV-Tools/issues/6307, https://github.com/Rust-GPU/rust-gpu/issues/373 and living on the bleeding edge.
struct MeshPayload
{
    int exponent;
};

const static float2 positions[3] = {
  float2(-0.5, 0.5),
  float2(0.5, 0.5),  
  float2(0.0, -0.5),
};

const static float3 colors[3] = {
  float3(1.0, 1.0, 0.0),
  float3(0.0, 1.0, 1.0),
  float3(1.0, 0.0, 1.0)
};

struct Vertex
{
  float4 pos : SV_Position;
  float3 color : Color;
};

const static uint MAX_VERTS = 12;
const static uint MAX_PRIMS = 4;

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(12, 1, 1)]
void meshMain(
    in uint tig : SV_GroupIndex,
    in payload MeshPayload meshPayload,
    OutputVertices<Vertex, MAX_VERTS> verts,
    OutputIndices<uint3, MAX_PRIMS> triangles)
{
    const uint numVertices = 12;
    const uint numPrimitives = 4;
    SetMeshOutputCounts(numVertices, numPrimitives);
    if(tig < numVertices)
    {
        verts[tig] = {float4(positions[tig % 3], 0, 1), colors[tig % 3]};
    }

    if(tig < numPrimitives)
        triangles[tig] = tig * 3 + uint3(0,1,2);
}
