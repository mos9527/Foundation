#define FAST_OBJ_IMPLEMENTATION
#include <fast_obj.h>

#include "Mesh.hpp"
#include <numeric>

void LoadObj(FImportedMesh& outMesh, StringView path)
{
    fastObjMesh* mesh = fast_obj_read(path.data());
    UniquePtr<fastObjMesh, decltype(&fast_obj_destroy)> raii(mesh, &fast_obj_destroy);
    CHECK_MSG(mesh, "Failed to load OBJ file: {}", path);
    uint32_t vertexBound = 0; // Bound post triangulation
    for (uint32_t face = 0; face < mesh->face_count; face++)
        vertexBound += 3u * (mesh->face_vertices[face] - 2u);
    outMesh.vertices.resize(vertexBound), outMesh.lods[0].indices.resize(vertexBound);
    for (size_t vtx = 0, idx = 0, face = 0; face < mesh->face_count; face++)
    {
        for (uint32_t faceVtx = 0; faceVtx < mesh->face_vertices[face]; faceVtx++)
        {
            auto [p, t, n] = mesh->indices[idx + faceVtx];
            if (faceVtx >= 3) // CCW
            {
                outMesh.vertices[vtx + 0] = outMesh.vertices[vtx - 3];
                outMesh.vertices[vtx + 1] = outMesh.vertices[vtx - 1];
                vtx += 2;
            }
            outMesh.vertices[vtx++] = {
                {mesh->positions[p * 3 + 0], mesh->positions[p * 3 + 1], mesh->positions[p * 3 + 2]},
                {mesh->normals[n * 3 + 0], mesh->normals[n * 3 + 1], mesh->normals[n * 3 + 2]},
                {},
                {}, // No tangent info
                {mesh->texcoords[t * 2 + 0], mesh->texcoords[t * 2 + 1]}};
        }
        idx += mesh->face_vertices[face];
    }
    std::iota(outMesh.lods[0].indices.begin(), outMesh.lods[0].indices.end(), 0);
}
