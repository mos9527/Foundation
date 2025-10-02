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
    fmt::print("lods: {}\n", data.lods.size());
    return 0;
}