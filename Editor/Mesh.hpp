#pragma once
#include <Core/Container.hpp>
#include <Math/Math.hpp>
using namespace Foundation;
using namespace Core;
using namespace Math;
constexpr uint32_t kMeshletMaxVertices = 64;  // max vertices per meshlet
constexpr uint32_t kMeshletMaxTriangles = 96; // max triangles per meshlet (indices=3*triangles)
#pragma pack(push, 1)
struct FVertex {
    float3 position;
    float3 normal;
    float2 uv;
};
static_assert(sizeof(FVertex) == 32); // TODO: Quantization
struct FMeshlet // same layout as meshopt_Meshlet
{
    /* offsets within meshletVtx and meshletTri arrays with meshlet data */
    uint32_t vtxOffset;  // in vertices
    uint32_t triOffset; // in indices (3*triangles)
    /* number of vertices and triangles used in the meshlet; data is stored in consecutive range defined by offset and count */
    uint32_t vtxCount;
    uint32_t triCount;
    /* bounds */
    float4 centerRadius; // (x,y,z,r)
    float4 coneAxisAngle; // (x,y,z,cos(half solid angle))
    float3 coneApex; // (x,y,z)
};
static_assert(sizeof(FMeshlet) == 60);
#pragma pack(pop)
struct FMesh {
    Vector<FVertex> vertices;
    struct LOD {
        Vector<uint32_t> indices;
        Vector<FMeshlet> meshlets;
        Vector<uint32_t> meshletVtx;
        Vector<uint8_t> meshletTri;
        LOD(Allocator* alloc) : indices(alloc), meshlets(alloc), meshletVtx(alloc), meshletTri(alloc) {}
    };
    Vector<LOD> lods;

    FMesh(Allocator* alloc);
    void Load(StringView path, bool optimize = true);
    // Creates N LOD levels, iteratively scaling down by 'scale' factor
    void SimplifyLOD(int levels, float scale);
    // Populates meshlet data for all LODs
    void Clusterize();
};

