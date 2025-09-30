#include <iostream>
#include <Assets/Mesh.hpp>
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
    Vector<MeshMeshlet> meshlet(allocator.Ptr());
    Vector<uint32_t> meshletVertices(allocator.Ptr()); // References to vertices
    Vector<uint8_t> meshletTriangles(allocator.Ptr()); // References to meshletVertices
    meshBuildMeshlets(meshlet, meshletVertices, meshletTriangles, vertices, indices);
    for (const auto& m : meshlet)
        fmt::print("Meshlet: vtx {} tri {}\n", m.vertexCount, m.triangleCount);
    return 0;
}