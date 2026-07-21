// Definitions From C++ sources
static const size_t kNumMeshDiscreteLODs = 5;
struct FVertex {
    float3 position;
    float3 normal;
    float3 tangent;
    float bitangentSign;
    float2 uv;
};
struct FLODGroup // @ref clodGroup
{
    // DAG level the group was generated at
    int depth;

    // sphere bounds, in mesh coordinate space
    float3 center;
    float radius;

    // combined simplification error, in mesh coordinate space
    // FLT_MAX for terminal groups
    float error;
};
struct FMeshlet // @ref meshopt_Meshlet
{
    /* meshlet group */
    /* ID of the @ref FLODGroup this meshlet belongs to in a hierarchy */
    uint32_t group;
    /* ID of the @ref FLODGroup (with more triangles) that produced this meshlet during simplification (parent). ~0u if original geometry */
    uint32_t refined;

    /* meshlet */
    /* offsets within meshletVtx and meshletTri arrays with meshlet data */
    uint32_t vtxOffset; // in vertices
    uint32_t triOffset; // in indices (3*triangles)
    /* number of vertices and triangles used in the meshlet; data is stored in consecutive range defined by offset and
     * count */
    uint32_t vtxCount;
    uint32_t triCount;

    /* bounds */
    float4 centerRadius; // (x,y,z,r)
    float4 coneAxisAngle; // (x,y,z,cos(half solid angle))
    float3 coneApex; // (x,y,z)
};
struct GSOffsetCount
{
    uint32_t offset;
    uint32_t count;
};
struct GSMesh
{
    GSOffsetCount vertices;
    GSOffsetCount indices;
    GSOffsetCount groups;
    GSOffsetCount meshlets;
    uint32_t meshletVtxOffset;
    uint32_t meshletTriOffset;
    uint32_t meshletGlobalIndex;
};
struct MIndex {
    uint8_t v0;
    uint8_t v1;
    uint8_t v2;
}
