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
    float2 uv;
};
static_assert(sizeof(FVertex) == 32); // TODO: Quantization
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
static_assert(sizeof(FMeshlet) == 68);

struct FMesh
{
    Vector<FVertex> vertices;
    struct LOD
    {
        Vector<uint32_t> indices;
        Vector<FMeshlet> meshlets;
        Vector<uint32_t> meshletVtx;
        Vector<uint8_t> meshletTri;
        LOD(Allocator* alloc) : indices(alloc), meshlets(alloc), meshletVtx(alloc), meshletTri(alloc) {}
    };
    Vector<LOD> lods;
    struct DAG
    {
        struct Cluster
        {
            uint32_t group{~0u}; // ID of the FLODGroup this cluster belongs to
            uint32_t refined{~0u}; // ID of the FLODGroup (with more triangles) that produced this cluster during simplification (parent). ~0u if original geometry
            Vector<uint32_t> indices;
            Cluster(Allocator* alloc) : indices(alloc) {}
        };
        Vector<Cluster> clusters; // Note: scratch buffer
        // -- final DAG data
        Vector<FLODGroup> groups; // group error bounds
        Vector<FMeshlet> meshlets; // meshlets built from all clusters
        Vector<uint32_t> meshletVtx;
        Vector<uint8_t> meshletTri;
        DAG(Allocator* alloc) : clusters(alloc), groups(alloc), meshlets(alloc), meshletVtx(alloc), meshletTri(alloc) {}
    } dag;

    FMesh(Allocator* alloc);
    /**
     * @brief Load OBJ mesh from file
     */
    void Load(StringView path, bool optimize = true);
    /**
    * @brief Creates N LOD levels, iteratively scaling down by 'scale' factor
    * @note Can be clusterized by @ref ClusterizeLOD afterward to create discrete LOD levels
    */
    void SimplifyLOD(int levels, float scale);
    /**
    * @brief Populates meshlet data for all LODs
    */
    void ClusterizeLOD();
    /**
    * @brief Partitions the clusters of LOD levels into a DAG
    * @note This enables continuous LOD behaviour, and as such should NOT be used with discrete LOD
    *       levels created by @ref ClusterizeLOD, or @ref SimplifyLOD.
    */
    void ClusterizeDAG();
    /**
     * @brief Returns an upper bound estimate of the size of the used mesh data when uploaded to GPU
     */
    [[nodiscard]] size_t ApproximateSize() const;
};
