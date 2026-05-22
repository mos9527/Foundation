#pragma once
#include <Core/Container.hpp>
#include <Math/Math.hpp>
#include "Serialization.hpp"
using namespace Foundation;
using namespace Core;
using namespace Math;

constexpr uint32_t kMeshletMaxVertices = 64; // max vertices per meshlet
constexpr uint32_t kMeshletMaxTriangles = 96; // max triangles per meshlet (indices=3*triangles)

#pragma pack(push,4)
struct FVertex
{
    float3 position;
    float3 normal;
    float3 tangent;
    float bitangentSign;
    float2 uv;
};
static_assert(sizeof(FVertex) == 48);

struct FQVertex
{
    uint16_t position[4]; // quantized FP16 [xyz] padding [w]
    uint32_t tbn32; // packed tangent frame
    uint16_t uv[2]; // quantized FP16 [uv]

    static uint32_t PackTBN(const float3& normal, const float3& tangent, float bitangentSign);
    static void UnpackTBN(uint32_t packed, float3& outNormal, float3& outTangent, float& outBitangentSign);

    static FQVertex Pack(FVertex const& vertex);
    static FVertex Unpack(FQVertex const& vertex);
};
static_assert(sizeof(FQVertex) == 16);
#pragma pack(pop)

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
static_assert(sizeof(FLODGroup) == 24);

struct FMeshlet // @ref meshopt_Meshlet
{
    /* meshlet group */
    /* ID of the @ref FLODGroup this meshlet belongs to in a hierarchy */
    uint32_t group;
    /* ID of the @ref FLODGroup (with more triangles) that produced this meshlet during simplification (parent). ~0u if
     * original geometry */
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
static_assert(sizeof(FMeshlet) == 68);

struct FSerializedMeshLOD
{
    FBlobRef indices;
    uint32_t indexCount{0};
};

struct FSerializedMesh
{
    FBlobRef vertices;
    uint32_t vertexCount{0};
    Vector<FSerializedMeshLOD> lods;
    FBlobRef dagGroups;
    FBlobRef dagMeshlets;
    FBlobRef dagMeshletTri;
    FBlobRef dagMeshletVtx;

    explicit FSerializedMesh(Allocator* alloc = GLOBAL_ALLOC)
        : lods(alloc)
    {
    }
};
