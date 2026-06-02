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

struct FImportedMesh
{
    Vector<FVertex> vertices; // Full precision, raw vertices. Used by importers.
    Vector<FQVertex> verticesQuantized; // Quantized vertex data for GPU upload.
    struct LOD
    {
        Vector<uint32_t> indices; // Full precision triangle indices
        LOD(Allocator* alloc) : indices(alloc) {}
    };
    Vector<LOD> lods;
    struct DAG
    {
        Vector<FLODGroup> groups; // LOD groups with error bounds
        Vector<FMeshlet> meshlets; // Meshlets built from all clusters
        Vector<uint8_t> meshletTri; // Meshlet local triangle indices
        Vector<uint32_t> meshletVtx; // Meshlet vertex indices into vertices/verticesQuantized
        DAG(Allocator* alloc) : groups(alloc), meshlets(alloc), meshletTri(alloc), meshletVtx(alloc) {}
    } dag;

    FImportedMesh(Allocator* alloc);
    /**
     * @brief Optimize vertex reuse with meshoptimizer
     */
    void Optimize();
    /**
     * @brief Creates N LOD levels, iteratively scaling down by 'scale' factor
     *        and populates @ref lods index data
     */
    void SimplifyLOD(int levels, float scale);
    /**
     * @brief Partitions the clusters of LOD levels into a DAG
     */
    void ClusterizeDAG();
    /**
     * @brief Quantizes vertex data into more compact representation
     *        Fills @ref quantizedVertices with quantized data from @ref rawVertices
     */
    void Quantize();
    /**
     * @brief Dequantizes quantized vertex back into full precision representation
     *        Fills @ref rawVertices with dequantized data from @ref quantizedVertices
     */
    void Dequantize();
    /**
     * @brief Prepares quantized GPU data buffers from possibly full-precision, or compressed data.
     * @return `true` when quantized and other buffers are available to be uploaded
     */
    bool EnsureQuantized();
    [[nodiscard]] bool IsQuantized() const { return !verticesQuantized.empty(); }
    /**
     * @brief Prepares full-precision data for CPU access
     */
    bool EnsureRaw();
    [[nodiscard]] bool IsRaw() const { return !vertices.empty(); }
    /**
     * @brief Returns a lower bound estimate of the size of the quantized mesh data.
     *        The size is conservative in that it's 100% accurate only when the data is written
     *        sequentially, without alignment requirements.
     * @note This is ONLY valid when @ref EnsureQuantized is true.
     */
    [[nodiscard]] size_t CalculateQuantizedBound(bool lod, bool dag) const;
};
/**
 * Loads a Wavefront OBJ file into a mesh, with no optimization applied
 */
void LoadObj(FImportedMesh& mesh, StringView path);

/* -- Math Exports -- */
void buildOrthonormalBasis(float3 n, float3& b1, float3& b2);
float2 packUnitOctahedralSnorm(float3 v);
float3 unpackUnitOctahedralSnorm(float2 v);
float packUnitCircleSnorm(float2 v);
float2 unpackUnitCircleSnorm(float v);
