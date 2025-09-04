#include "Mesh.hpp"
using namespace Foundation::Core;
using namespace Foundation::Blobs;

#define FAST_OBJ_IMPLEMENTATION
#include <fast_obj.h>
#include <meshoptimizer.h>

template<typename Vertex, typename Index>
void RunMeshOptimizerPass(StlVector<Vertex>& vertices, StlVector<Index>& indices, Allocator* allocator) {
    StlVector<uint32_t> remap(vertices.size(), allocator);
    size_t unique = meshopt_generateVertexRemap(remap.data(), indices.data(), indices.size(), vertices.data(), vertices.size(), sizeof(Vertex));
    meshopt_remapVertexBuffer(vertices.data(), vertices.data(), vertices.size(), sizeof(Vertex), remap.data());
    meshopt_remapIndexBuffer(indices.data(), indices.data(), indices.size(), remap.data());
    vertices.resize(unique);
    meshopt_optimizeVertexCache(indices.data(), indices.data(), indices.size(), vertices.size());
    meshopt_optimizeVertexFetch(vertices.data(), indices.data(), indices.size(), vertices.data(), vertices.size(), sizeof(Vertex));
}

// Reference: https://github.com/zeux/niagara/blob/master/src/scene.cpp
Mesh Foundation::Cooking::Cook<Mesh>::FromOBJ(std::filesystem::path const& path, Allocator* allocator)
{
    fastObjMesh* mesh = fast_obj_read(path.string().c_str());
    uint32_t num_vtx = 0;
    // No. of INDICES post triangulation w/o de-dupe
    // Since this will be optimized later on we can keep the worst possible
    // storage (dupe vertices used for tris are stored as is + unaccounted for original duplications)
    // for the VERTEX buffer here to store this as a flatten triangle list as is
    for (uint32_t face = 0; face < mesh->face_count; face++)
        num_vtx += 3u * (mesh->face_vertices[face] - 2u);
    StlVector<OBJVertex> vertices(num_vtx, allocator);
    StlVector<uint32_t> indices(num_vtx, allocator);
    std::iota(indices.begin(), indices.end(), 0);

    for (size_t vtx = 0, idx = 0, face = 0; face < mesh->face_count; face++) {
        for (uint32_t fvtx = 0; fvtx < mesh->face_vertices[face]; fvtx++) {
            fastObjIndex index = mesh->indices[idx + fvtx];
            /* Fan triangulation
                Requires input vertices being convex and already sorted/wound in CCW/CW
                fast_obj DOESN'T handle this. Though most DCCs do at their export stage. */
            if (fvtx >= 3) {
                // Assume CCW
                vertices[vtx + 0] = vertices[vtx - 3]; // vtx - 3 always points to the first                 
                vertices[vtx + 1] = vertices[vtx - 1]; // vertex in this face
                vtx += 2;
            }
            auto& v = vertices[vtx++];
            v.px = Math::QuantizeFP16(mesh->positions[index.p * 3 + 0]);
            v.py = Math::QuantizeFP16(mesh->positions[index.p * 3 + 1]);
            v.pz = Math::QuantizeFP16(mesh->positions[index.p * 3 + 2]);
            // No tangent
            v.tp = 0;
            Math::vec2 nor = Math::PackUnitOctahedral({
                mesh->normals[index.n * 3 + 0],
                mesh->normals[index.n * 3 + 1],
                mesh->normals[index.n * 3 + 2]
            });
            v.np = {
                .nx = Math::QuantizeSnormShifted(nor.x, 15),
                .ny = Math::QuantizeSnormShifted(nor.y, 15),
                .sign = 0
            };
            v.u = Math::QuantizeFP16(mesh->texcoords[index.t * 2 + 0]);
            v.v = Math::QuantizeFP16(mesh->texcoords[index.t * 2 + 1]);
        }
        idx += mesh->face_vertices[face];
    }
    fast_obj_destroy(mesh);
    RunMeshOptimizerPass(vertices, indices, allocator);
    return Mesh(
        { reinterpret_cast<const char*>(vertices.data()), vertices.size() * sizeof(OBJVertex) },
        { reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(OBJIndex) },
        vertices.size(), indices.size(),
        allocator
    );
}
