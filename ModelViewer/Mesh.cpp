#include <Math/Math.hpp>
#include <Native/Filesystem.hpp>
#include "Mesh.hpp"
using namespace Foundation;
using namespace Foundation::Core;
using namespace Foundation::Math;
using namespace Foundation::Native;

#define FAST_OBJ_IMPLEMENTATION
#include <fast_obj.h>
#include <meshoptimizer.h>

template<typename Vertex, typename Index>
void RunMeshOptimizerPass(Vector<Vertex>& vertices, Vector<Index>& indices, Allocator* allocator) {
    Vector<uint32_t> remap(vertices.size(), allocator);
    size_t unique = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(), vertices.data(), vertices.size(), sizeof(Vertex));
    meshopt_remapVertexBuffer(vertices.data(), vertices.data(), vertices.size(), sizeof(Vertex), remap.data());
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    vertices.resize(unique);
    meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertices.size());
    meshopt_optimizeVertexFetch(vertices.data(), indices.data(), indices.size(), vertices.data(), vertices.size(), sizeof(Vertex));
}

// Reference: https://github.com/zeux/niagara/blob/master/src/scene.cpp
Mesh LoadMeshFromObjFile(Path path, Core::Allocator* allocator)
{
    fastObjMesh* mesh = fast_obj_read(path.string().c_str());
    CHECK_MSG(mesh, "Failed to load OBJ file: {}", path.string());
    uint32_t num_vtx = 0;
    // Alloc for zero reuse scenario
    for (uint32_t face = 0; face < mesh->face_count; face++)
        num_vtx += 3u * (mesh->face_vertices[face] - 2u);
    Vector<Vertex> vertices(num_vtx, allocator);
    Vector<uint32_t> indices(num_vtx, allocator);
    std::iota(indices.begin(), indices.end(), 0);

    for (size_t vtx = 0, idx = 0, face = 0; face < mesh->face_count; face++) {
        for (uint32_t fvtx = 0; fvtx < mesh->face_vertices[face]; fvtx++) {
            fastObjIndex index = mesh->indices[idx + fvtx];
            // Fan triangulation               
            if (fvtx >= 3) {
                // Assume CCW
                vertices[vtx + 0] = vertices[vtx - 3]; // vtx - 3 always points to the first                 
                vertices[vtx + 1] = vertices[vtx - 1]; // vertex in this face
                vtx += 2;
            }
            auto& v = vertices[vtx++];
            v.px = quantizeFP16(mesh->positions[index.p * 3 + 0]);
            v.py = quantizeFP16(mesh->positions[index.p * 3 + 1]);
            v.pz = quantizeFP16(mesh->positions[index.p * 3 + 2]);
            // No tangent
            v.tp = 0;
            vec3 nor_orignal = {
                mesh->normals[index.n * 3 + 0],
                mesh->normals[index.n * 3 + 1],
                mesh->normals[index.n * 3 + 2]
            };
            vec2 nor = packUnitOctahedral(nor_orignal);
            v.np = {
                .nx = quantizeSnormShifted(nor.x, 15),
                .ny = quantizeSnormShifted(nor.y, 15),
                .sign = 0
            };
            v.u = quantizeFP16(mesh->texcoords[index.t * 2 + 0]);
            v.v = quantizeFP16(mesh->texcoords[index.t * 2 + 1]);
        }
        idx += mesh->face_vertices[face];
    }
    fast_obj_destroy(mesh);
    RunMeshOptimizerPass(vertices, indices, allocator);
    return Mesh(
        { reinterpret_cast<const char*>(vertices.data()), vertices.size() * sizeof(Vertex) },
        { reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(Index) },
        vertices.size(), indices.size(),
        allocator
    );
}
