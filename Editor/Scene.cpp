#define CGLTF_IMPLEMENTATION
#define CGLTF_VALIDATE_ENABLE_ASSERTS 1
#include <cgltf.h>

// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#meshes
FMesh LoadSubmesh(cgltf_primitive* submesh)
{
    CHECK(submesh->type == cgltf_primitive_type_triangles);
    FMesh mesh(GContext->allocator);
    // Vertex count would be the same across POSITION, NORMAL, etc. Getting any of those would be enough.
    size_t numVertices = submesh->attributes[0].data->count;
    // Worst storage case is VEC4
    {
        Vector<float> unpack(numVertices * 4, GContext->allocator);
        mesh.vertices.resize(numVertices);
        if (auto acc = cgltf_find_accessor(submesh, cgltf_attribute_type_position, 0))
        {
            cgltf_accessor_unpack_floats(acc, unpack.data(), numVertices * 3); // VEC3
            for (size_t i = 0; i < numVertices; i++)
            {
                auto& vtx = mesh.vertices[i];
                vtx.position = {unpack[i * 3 + 0], unpack[i * 3 + 1], unpack[i * 3 + 2]};
            }
        }
        if (auto acc = cgltf_find_accessor(submesh, cgltf_attribute_type_normal, 0))
        {
            cgltf_accessor_unpack_floats(acc, unpack.data(), numVertices * 3); // VEC3
            for (size_t i = 0; i < numVertices; i++)
            {
                auto& vtx = mesh.vertices[i];
                vtx.normal = {unpack[i * 3 + 0], unpack[i * 3 + 1], unpack[i * 3 + 2]};
            }
        }
        if (auto acc = cgltf_find_accessor(submesh, cgltf_attribute_type_texcoord, 0))
        {
            cgltf_accessor_unpack_floats(acc, unpack.data(), numVertices * 2); // VEC3
            for (size_t i = 0; i < numVertices; i++)
            {
                auto& vtx = mesh.vertices[i];
                vtx.uv = {unpack[i * 2 + 0], unpack[i * 2 + 1]};
            }
        }
    }
    size_t numIndices = submesh->indices->count;
    auto& m0 = mesh.lods[0];
    m0.indices.resize(numIndices);
    cgltf_accessor_unpack_indices(submesh->indices, m0.indices.data(), sizeof(uint32_t), numIndices);
    return mesh;
}

void LoadGLTF(StringView path, Vector<FMesh>& outMeshes, Vector<FInstance>& outInstances)
{
    LOG(Scene, LogInfo, "Load GLTF Scene {}", path);
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    UniquePtr<cgltf_data, decltype(&cgltf_free)> raii(data, &cgltf_free);
    {
        cgltf_result result = cgltf_parse_file(&options, path.data(), &data);
        CHECK_MSG(result == cgltf_result_success, "Scene load failure: {}", static_cast<int>(result));
        result = cgltf_load_buffers(&options, data, path.data());
        CHECK_MSG(result == cgltf_result_success, "Buffer load failure: {}", static_cast<int>(result));
        result = cgltf_validate(data);
        CHECK_MSG(result == cgltf_result_success, "Scene validate failure: {}", static_cast<int>(result));
    }
    /* Meshes */
    size_t numSubmeshes = 0;
    for (size_t i = 0; i < data->meshes_count; i++)
        numSubmeshes += data->meshes[i].primitives_count;
    // Mesh's submesh children
    // These will be flattened later on into instances of themselves
    Vector<Vector<size_t>> submeshIndices(GContext->allocator);
    outMeshes.resize(numSubmeshes, GContext->allocator);
    LOG(Scene, LogInfo, "Loading meshes. Sub total={}", numSubmeshes);
    ThreadPool pool(std::thread::hardware_concurrency(), 4096, GContext->allocator, "meshopt");
    for (size_t mi = 0, i = 0; i < data->meshes_count; i++)
    {
        auto& mesh = data->meshes[i];
        auto& subs = submeshIndices.emplace_back(GContext->allocator);
        for (size_t p = 0; p < mesh.primitives_count; p++)
        {
            auto* sub = mesh.primitives + p;
            if (sub->type == cgltf_primitive_type_triangles)
            {
                pool.Push(
                    [&](size_t index)
                    {
                        auto& submesh = outMeshes[index];
                        submesh = LoadSubmesh(sub);
                        submesh.Optimize();
                        submesh.ClusterizeDAG();
                    }, mi++);
            }
            subs.emplace_back(p);
        }
    }
    /* Instances */
    for (size_t i = 0; i < data->nodes_count; i++)
    {
        const cgltf_node* node = &data->nodes[i];
        if (node->mesh)
        {
            mat4 world;
            cgltf_node_transform_world(node, reinterpret_cast<float*>(&world));
            FInstance instance{};
            float3 skew; float4 presp; // unused
            decompose(world, instance.scale, instance.rotation, instance.transform, skew, presp);
            auto meshIndex = cgltf_mesh_index(data, node->mesh);
            for (auto sub : submeshIndices[meshIndex])
            {
                instance.meshIndex = sub;
                outInstances.emplace_back(instance);
            }
        }
    }
    pool.Join();
}
