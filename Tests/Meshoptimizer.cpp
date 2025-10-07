#include <Assets/Scene.hpp>
#include <Core/DefaultAllocator.hpp>
using namespace ModelViewer;
int main()
{
    DefaultAllocator allocator;

    Native::Path path("data/assets/Sphere.obj");
    Vector<MeshVertex> vertices(allocator.Ptr());
    Vector<MeshIndex> indices(allocator.Ptr());
    meshLoadObjFile(vertices, indices, path);
    fmt::print("vtx: {} idx {}\n", vertices.size(), indices.size());
    auto data = sceneMeshDataFromVertexIndex(vertices, indices, &allocator, 4, true);
    auto& mesh = data.lods[0];
    fmt::print("lod0: vtx: {} idx {} meshlets {} mVtx {} mTri {}\n", data.vertices.size(), mesh.indices.size(),
               mesh.meshlets.size(), mesh.meshletVertices.size(), mesh.meshletTriangles.size());
    for (auto& meshlet : mesh.meshlets)
    {
        fmt::print("  m: vtxOff {} vtxCnt {} triOff {} triCnt {}\n", meshlet.vertexOffset, meshlet.vertexCount, meshlet.triangleOffset, meshlet.triangleCount);
        for (uint32_t ti = 0; ti < meshlet.triangleCount; ti++)
        {
            uint32_t t = ti * 3 + meshlet.triangleOffset;
            fmt::print("    tri: {}, {}, {}\n", mesh.meshletTriangles[t], mesh.meshletTriangles[t+1],mesh.meshletTriangles[t+2]);
        }
    }
    return 0;
}