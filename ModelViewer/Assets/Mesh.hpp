#pragma once
#include <Native/Filesystem.hpp>
#include <RHICore/Common.hpp>
#include <Core/Core.hpp>
#include <Math/Math.hpp>

namespace ModelViewer
{
    using namespace Foundation;
    using namespace Math;
    using namespace Core;
    using MeshIndex = uint32_t; // 32-bit index
    /**
     * @brief Full-size vertex structure for static meshes
     */
    struct MeshVertex
    {
        vec3 pos;
        vec3 normal;
        vec3 tangent;
        vec3 bitangent;
        vec2 uv;
    };
    /**
     * @brief Compact 4-byte aligned vertex structure for static meshes
     */
    struct MeshVertexCompact {
        uint16_t px, py, pz; // quantized fp16
        struct // 16 bits
        {
            uint32_t tx : 8; // packed x
            uint32_t ty : 8; // packed y
        } tp;         // tangent [octa 8+8]
        struct { // 32 bits
            uint32_t nx : 15;  // packed x
            uint32_t ny : 15;  // packed y
            uint32_t sign : 2; // bitangent sign
        } np; // normal packed [snorm octa 15+15, bitangent sign 2]
        uint16_t u, v;       // UV coords fp16
        /**
         * @brief Pack vertex attributes into compact MeshVertex
         */
        static MeshVertexCompact Pack(vec3 pos, vec3 normal, vec3 tangent, vec3 bitangent, vec2 uv);
        static MeshVertexCompact Pack(MeshVertex data)
        {
            return Pack(data.pos, data.normal, data.tangent, data.bitangent, data.uv);
        }
        /**
         * @brief Unpack vertex attributes from compact MeshVertex
         */
        void Unpack(vec3& pos, vec3& normal, vec3& tangent, vec3& bitangent, vec2& uv) const;
        void Unpack(MeshVertex& data) const
        {
            Unpack(data.pos, data.normal, data.tangent, data.bitangent, data.uv);
        }
    };
    /**
     * @brief Meshlet structure containing offsets and counts to access meshlet data
     * @note Reference: https://github.com/zeux/meshoptimizer?tab=readme-ov-file#clusterization
     */
    struct MeshMeshlet // same layout as meshopt_Meshlet
    {
        /* offsets within meshlet_vertices and meshlet_triangles arrays with meshlet data */
        uint32_t vertexOffset;
        uint32_t triangleOffset;
        /* number of vertices and triangles used in the meshlet; data is stored in consecutive range defined by offset and count */
        uint32_t vertexCount;
        uint32_t triangleCount;
    };
    using MeshMicroIndex = uint8_t; // 8-bit index into meshlet vertex array
    constexpr uint32_t kMeshletMaxVertices = 64;  // max vertices per meshlet
    constexpr uint32_t kMeshletMaxTriangles = 32 * 3; // max triangle indices per meshlet (96, 3 per)
    /**
     * @brief Build meshlets from a mesh
     * @param outMeshlet Output meshlet array
     * @param outMeshletVertices Output meshlet vertex indirection array
     * @param outMeshletTriangles Output meshlet triangle index array (into meshlet vertices)
     * @param vertices Input vertex array
     * @param indices Input index array
     */
    void meshBuildMeshlets(Vector<MeshMeshlet>& outMeshlet, Vector<MeshIndex>& outMeshletVertices, Vector<MeshMicroIndex>& outMeshletTriangles, Span<const MeshVertex> vertices, Span<const MeshIndex> indices);
    /**
     * @brief Generate a simplified LOD mesh index buffer from the input mesh
     * @param outIndices Output index array for the LOD mesh
     * @param vertices Input vertex array
     * @param indices Input source index array.
     * @param lodScale Scale factor for the LOD (0.0 - 1.0)
     * @return Error factor of this LOD to the input mesh
     */
    float meshGenerateLod(Vector<MeshIndex>& outIndices, Span<const MeshVertex> vertices, Span<const MeshIndex> indices, float lodScale, Allocator* allocator);
    /**
     * @brief Load a mesh from an OBJ file
     * @param outVertex Output vertex array
     * @param outIndex Output index array
     * @param path Path to the OBJ file
     */
    void meshLoadObjFile(Vector<MeshVertex>& outVertex, Vector<MeshIndex>& outIndex, const Native::Path& path);
}
