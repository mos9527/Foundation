#include "Mesh.hpp"
#include <Core/DefaultAllocator.hpp>
#include <Math/Math.hpp>
#include <Native/Filesystem.hpp>

namespace ModelViewer
{
    using namespace Foundation;
    using namespace Foundation::Core;
    using namespace Foundation::Math;
    using namespace Foundation::Native;

    MeshVertexCompact MeshVertexCompact::Pack(const vec3 pos, const vec3 normal, const vec3 tangent,
                                              const vec3 bitangent, const vec2 uv)
    {
        MeshVertexCompact v;
        v.px = quantizeFP16(pos.x);
        v.py = quantizeFP16(pos.y);
        v.pz = quantizeFP16(pos.z);
        vec2 tangentOct = packUnitOctahedral(tangent);       
        v.tp = quantizeSnormShifted(tangentOct.x, 8) << 8u | quantizeSnormShifted(tangentOct.y, 8);
        vec2 normalOct = packUnitOctahedral(normal);
        uint32_t sgn = dot(cross(normal, tangent), bitangent) < 0;
        v.np = quantizeSnormShifted(normalOct.x, 15) << 15u | quantizeSnormShifted(normalOct.y, 15) << 2u | sgn;
        v.u = quantizeFP16(uv.x);
        v.v = quantizeFP16(uv.y);
        return v;
    }

    // meshoptimizer doesn't provide user data for allocators - we'd use a global one
    // here for all temporary allocations that might happen in the functions below.
    static DefaultAllocator MeshoptAllocator;
    static void* meshoptAlloc(size_t size) { return MeshoptAllocator.Allocate(size, alignof(std::max_align_t)); }
    static void meshoptFree(void* ptr) { MeshoptAllocator.Deallocate(ptr); }
#include <meshoptimizer.h>
    template <typename Vertex, typename Index>
    void optimizeVertexIndex(Vector<Vertex>& vertices, Vector<Index>& indices)
    {
        meshopt_setAllocator(meshoptAlloc, meshoptFree);
        Vector<uint32_t> remap(vertices.size(), MeshoptAllocator.Ptr());
        size_t unique = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(), vertices.data(),
                                                    vertices.size(), sizeof(Vertex));
        meshopt_remapVertexBuffer(vertices.data(), vertices.data(), vertices.size(), sizeof(Vertex), remap.data());
        meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
        vertices.resize(unique);
        meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertices.size());
        meshopt_optimizeVertexFetch(vertices.data(), indices.data(), indices.size(), vertices.data(), vertices.size(),
                                    sizeof(Vertex));
    }

    float meshGenerateLod(Vector<MeshIndex>& outIndices, Span<const MeshVertex> vertices,
                           Span<const MeshIndex> indices, const float lodScale)
    {
        meshopt_setAllocator(meshoptAlloc, meshoptFree);
        const float normalWeight = 0.5f;
        const float attrWeights[3] = {normalWeight, normalWeight, normalWeight};
        auto targetIndexCount = static_cast<uint32_t>(ceilf(indices.size() * lodScale));
        outIndices.resize(indices.size());
        float actualError;
        size_t lodSize = meshopt_simplifyWithAttributes(
            outIndices.data(), indices.data(), indices.size(), &vertices[0].pos.x, vertices.size(), sizeof(MeshVertex),
            &vertices[0].normal.x, sizeof(MeshVertex), attrWeights, 3, nullptr, targetIndexCount,
            1e-1f, // max error
            0, // options
            &actualError);
        outIndices.resize(lodSize);
        return actualError;
    }

    void meshBuildMeshlets(Vector<MeshMeshlet>& outMeshlet, Vector<MeshIndex>& outMeshletVertices,
                           Vector<MeshMicroIndex>& outMeshletTriangles, Span<const MeshVertex> vertices,
                           Span<const MeshIndex> indices)
    {
        meshopt_setAllocator(meshoptAlloc, meshoptFree);
        size_t maxMeshlets = meshopt_buildMeshletsBound(indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles);
        outMeshlet.resize(maxMeshlets);
        // Resize to worst bounds first
        outMeshletVertices.resize(maxMeshlets * kMeshletMaxVertices);
        outMeshletTriangles.resize(maxMeshlets * kMeshletMaxTriangles);
        static_assert(sizeof(MeshMeshlet) == sizeof(meshopt_Meshlet));
        static_assert(offsetof(MeshMeshlet, vertexOffset) == offsetof(meshopt_Meshlet, vertex_offset));
        static_assert(offsetof(MeshMeshlet, triangleOffset) == offsetof(meshopt_Meshlet, triangle_offset));
        static_assert(offsetof(MeshMeshlet, vertexCount) == offsetof(meshopt_Meshlet, vertex_count));
        static_assert(offsetof(MeshMeshlet, triangleCount) == offsetof(meshopt_Meshlet, triangle_count));
        size_t meshlets =
            meshopt_buildMeshlets(reinterpret_cast<meshopt_Meshlet*>(outMeshlet.data()), outMeshletVertices.data(),
                                  outMeshletTriangles.data(), indices.data(), indices.size(), &vertices[0].pos.x,
                                  vertices.size(), sizeof(MeshVertex), kMeshletMaxVertices, kMeshletMaxTriangles,
                                  0.25f // As recommended by the docs
            );
        outMeshlet.resize(meshlets);
        {
            const auto& [vertexOffset, triangleOffset, vertexCount, triangleCount] = outMeshlet.back();
            outMeshletVertices.resize(vertexOffset + vertexCount);
            outMeshletTriangles.resize(triangleOffset + triangleCount * 3);
        }
    }

#define FAST_OBJ_IMPLEMENTATION
#include <fast_obj.h>

    // Reference: https://github.com/zeux/niagara/blob/master/src/scene.cpp
    void meshLoadObjFile(Vector<MeshVertex>& vertices, Vector<MeshIndex>& indices, const Path& path)
    {
        fastObjMesh* mesh = fast_obj_read(path.string().c_str());
        CHECK_MSG(mesh, "Failed to load OBJ file: {}", path);
        uint32_t num_vtx = 0;
        // Alloc for zero reuse scenario where all vertices are assumed to be unique
        for (uint32_t face = 0; face < mesh->face_count; face++)
            num_vtx += 3u * (mesh->face_vertices[face] - 2u);
        vertices.resize(num_vtx), indices.resize(num_vtx);
        std::iota(indices.begin(), indices.end(), 0);
        for (size_t vtx = 0, idx = 0, face = 0; face < mesh->face_count; face++)
        {
            for (uint32_t faceVtx = 0; faceVtx < mesh->face_vertices[face]; faceVtx++)
            {
                auto [p, t, n] = mesh->indices[idx + faceVtx];
                // Fan triangulation
                if (faceVtx >= 3)
                {
                    // Assume CCW
                    vertices[vtx + 0] = vertices[vtx - 3]; // vtx - 3 always points to the first
                    vertices[vtx + 1] = vertices[vtx - 1]; // vertex in this face
                    vtx += 2;
                }
                vertices[vtx++] = {{mesh->positions[p * 3 + 0], mesh->positions[p * 3 + 1], mesh->positions[p * 3 + 2]},
                                   {mesh->normals[n * 3 + 0], mesh->normals[n * 3 + 1], mesh->normals[n * 3 + 2]},
                                   {},
                                   {}, // no tangent information
                                   {mesh->texcoords[t * 2 + 0], mesh->texcoords[t * 2 + 1]}};
            }
            idx += mesh->face_vertices[face];
        }
        fast_obj_destroy(mesh);
        optimizeVertexIndex(vertices, indices);
    }
} // namespace ModelViewer
