#pragma once
#include <Renderer/Mesh.hpp>

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