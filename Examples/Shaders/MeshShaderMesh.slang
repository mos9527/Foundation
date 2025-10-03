// Q: Why is this seperated into *3* different files?
// A: https://github.com/KhronosGroup/SPIRV-Tools/issues/6307, https://github.com/Rust-GPU/rust-gpu/issues/373 and living on the bleeding edge.
struct MeshPayload
{
    float time;
};

const static float2 positions[3] = {
  float2(0, 1),  
  float2(1, -1),
  float2(-1, -1),    
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

const static uint MAX_VERTS = 3;
const static uint MAX_PRIMS = 1;

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(3, 1, 1)] // Each WG generates one triangle
void meshMain(
    in uint meshlet: SV_GroupID, // Task dispatches 64 meshlets
    in uint tig : SV_GroupIndex,
    in payload MeshPayload meshPayload,
    OutputVertices<Vertex, MAX_VERTS> verts,
    OutputIndices<uint3, MAX_PRIMS> triangles)
{
    float px = meshlet / 8, py = meshlet % 8;
    px /= 8.0, py /= 8.0;    
    float2 scale = float2(1 / 8.0, 1 / 8.0);
    float2 translation = (float2(px, py) * 2.0 - 1.0);
    const uint numVertices = 3;
    const uint numPrimitives = 1;
    SetMeshOutputCounts(numVertices, numPrimitives);
    if(tig < numVertices)
    {
        float3 vert = float3((positions[tig % 3] + translation) * scale, 1);  
        vert += float3(sin(meshPayload.time + meshlet) * meshlet / 64, cos(meshPayload.time + meshlet) * meshlet / 64, 0);
        verts[tig] = {float4(vert, 1), colors[tig % 3]};
    }
    triangles[0] = uint3(0,1,2);
}
