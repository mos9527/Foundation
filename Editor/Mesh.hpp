#pragma once
#include <Core/Container.hpp>
#include <Math/Math.hpp>
using namespace Foundation;
using namespace Core;
using namespace Math;
constexpr uint32_t kMeshletMaxVertices = 64; // max vertices per meshlet
constexpr uint32_t kMeshletMaxTriangles = 96; // max triangles per meshlet (indices=3*triangles)
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
    uint16_t position[3]; // quantized FP16
    uint32_t tbn32; // Octahedral normal [10+10] + tangent rotation [10] + bitangent sign [2]
    uint16_t uv[2]; // quantized UNORM16

    static uint32_t PackTBN(const float3& normal, const float3& tangent, float bitangentSign);
    static void UnpackTBN(uint32_t packed, float3& outNormal, float3& outTangent, float& outBitangentSign);

    static FQVertex Pack(FVertex const& vertex);
    static FVertex Unpack(FQVertex const& vertex);
};
static_assert(sizeof(FQVertex) == 16);
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

struct FMesh
{
    Vector<FVertex> vertices; // Full precision, raw vertices. Used by importers.
    Vector<FQVertex> verticesQuantized; // Quantized vertex data for GPU upload.
    Vector<unsigned char> verticesQuantizedCompressed; // Compressed, quantized post-optimization vertex data for disk.
    uint32_t verticesQuantizedCompressedCount = 0; // Number of vertices in compressed data
    struct LOD
    {
        Vector<uint32_t> indices; // Full precision triangle indices
        Vector<unsigned char> indicesCompressed; // Compressed index data for disk
        uint32_t indicesCompressedCount = 0; // Number of indices in compressed data
        LOD(Allocator* alloc) : indices(alloc), indicesCompressed(alloc) {}
    };
    Vector<LOD> lods;
    struct DAG
    {
        Vector<FLODGroup> groups; // LOD groups with error bounds
        Vector<FMeshlet> meshlets; // Meshlets built from all clusters
        Vector<uint8_t> meshletTri; // Meshlet local triangle indices
        Vector<uint32_t> meshletVtx; // Meshlet vertex indices into vertices/verticesQuantized
        Vector<unsigned char> meshletVtxCompressed; // Compressed meshlet vertex indices for disk
        uint32_t meshletVtxCompressedCount = 0; // Number of indices in compressed data
        DAG(Allocator* alloc) : groups(alloc), meshlets(alloc), meshletTri(alloc), meshletVtx(alloc), meshletVtxCompressed(alloc) {}
    } dag;

    FMesh(Allocator* alloc);
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
     * @brief Compresses quantized vertex and index data for disk storage
     */
    void Compress();
    /**
     * @brief Decompresses quantized vertex and index data from disk storage
     */
    void Decompress();
    /**
     * @brief Prepares quantized GPU data buffers from possibly full-precision, or compressed data.
     * @return `true` when quantized and other buffers are available to be uploaded
     */
    bool EnsureQuantized();
    [[nodiscard]] bool IsQuantized() const { return !verticesQuantized.empty(); }
    /**
     * @brief Prepares compressed buffers for saving to disk.
     * @note  A compressed mesh implies quantized data.
     */
    bool EnsureCompressed();
    [[nodiscard]] bool IsCompressed() const { return !verticesQuantizedCompressed.empty(); }
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

void LoadObj(FMesh& mesh, StringView path);
