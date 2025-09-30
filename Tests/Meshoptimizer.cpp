#include <algorithm>
#include <numeric>
#include <vector>

#define FAST_OBJ_IMPLEMENTATION
#include <fast_obj.h>
#include <meshoptimizer.h>

struct Vec3
{
    float x, y, z;
};
// https://github.com/zeux/niagara/blob/master/src/scene.cpp#L109
static bool loadObj(std::vector<Vec3>& positions, const char* path)
{
    fastObjMesh* obj = fast_obj_read(path);
    if (!obj)
        return false;

    size_t index_count = 0;

    for (unsigned int i = 0; i < obj->face_count; ++i)
        index_count += 3 * (obj->face_vertices[i] - 2);

    positions.resize(index_count);

    size_t vertex_offset = 0;
    size_t index_offset = 0;

    for (unsigned int i = 0; i < obj->face_count; ++i)
    {
        for (unsigned int j = 0; j < obj->face_vertices[i]; ++j)
        {
            fastObjIndex gi = obj->indices[index_offset + j];

            // triangulate polygon on the fly; offset-3 is always the first polygon vertex
            if (j >= 3)
            {
                positions[vertex_offset + 0] = positions[vertex_offset - 3];
                positions[vertex_offset + 1] = positions[vertex_offset - 1];
                vertex_offset += 2;
            }

            Vec3& vtx = positions[vertex_offset++];
            vtx.x = obj->positions[gi.p * 3 + 0];
            vtx.y = obj->positions[gi.p * 3 + 1];
            vtx.z = obj->positions[gi.p * 3 + 2];
        }

        index_offset += obj->face_vertices[i];
    }

    assert(vertex_offset == index_count);

    fast_obj_destroy(obj);

    return true;
}
#define INDEX_BOUNDS
int main()
{
    std::vector<Vec3> vertices;
    loadObj(vertices, "Sphere.obj");

    std::vector<unsigned int> remap(vertices.size()); // temporary remap table
    size_t unique = meshopt_generateVertexRemap(&remap[0], NULL, vertices.size(),
        &vertices[0], vertices.size(), sizeof(Vec3));
    std::vector<unsigned int> indices(vertices.size());
    meshopt_remapIndexBuffer(indices.data(), NULL, vertices.size(), &remap[0]);
    meshopt_remapVertexBuffer(vertices.data(), vertices.data(), vertices.size(), sizeof(Vec3), &remap[0]);
    vertices.resize(unique);

    const size_t max_vertices = 64;
    const size_t max_triangles = 96;
    const float cone_weight = 0.25f;

    std::vector<meshopt_Meshlet> meshlets(meshopt_buildMeshletsBound(indices.size(), max_vertices, max_triangles));
#ifdef INDEX_BOUNDS
    std::vector<unsigned int> meshlet_vertices(indices.size());
    std::vector<unsigned char> meshlet_triangles(indices.size());
#else
    std::vector<unsigned int> meshlet_vertices(meshlets.size() * max_vertices);
    std::vector<unsigned char> meshlet_triangles(meshlets.size() * max_triangles * 3);
#endif

    meshlets.resize(meshopt_buildMeshlets(meshlets.data(), meshlet_vertices.data(), meshlet_triangles.data(),
                                          indices.data(), indices.size(), &vertices[0].x, vertices.size(), sizeof(Vec3),
                                          max_vertices, max_triangles, cone_weight));

    return 0;
}