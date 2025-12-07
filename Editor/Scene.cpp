#define CGLTF_IMPLEMENTATION
#define CGLTF_VALIDATE_ENABLE_ASSERTS 1
#include "Scene.hpp"
#include <Math/Decompose.hpp>
#include <cgltf.h>
#include <filesystem>
#include <fstream>
#include "Mesh.hpp"
// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#meshes
FMesh SceneLoadGLTFSubmesh(cgltf_primitive* submesh)
{
    CHECK(submesh->type == cgltf_primitive_type_triangles);
    FMesh mesh(GLOBAL_ALLOC);
    // Vertex count would be the same across POSITION, NORMAL, etc. Getting any of those would be enough.
    size_t numVertices = submesh->attributes[0].data->count;
    // Worst storage case is VEC4
    {
        Vector<float> unpack(numVertices * 4, GLOBAL_ALLOC);
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
        if (auto acc = cgltf_find_accessor(submesh, cgltf_attribute_type_tangent, 0))
        {
            cgltf_accessor_unpack_floats(acc, unpack.data(), numVertices * 4); // VEC3 + sign
            for (size_t i = 0; i < numVertices; i++)
            {
                auto& vtx = mesh.vertices[i];
                vtx.tangent = {unpack[i * 4 + 0], unpack[i * 4 + 1], unpack[i * 4 + 2]};
                vtx.bitangentSign = unpack[i * 4 + 3];
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

void SceneLoadGLTF(StringView path, Vector<FMesh>& outMeshes, Vector<FInstance>& outInstances,
                   Vector<FCamera>& outCameras)
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
    Vector<Pair<size_t, size_t>> submeshIndices(GLOBAL_ALLOC);
    outMeshes.resize(numSubmeshes, GLOBAL_ALLOC);
    LOG(Scene, LogInfo, "Loading meshes. Sub total={}", numSubmeshes);
    ThreadPool pool(std::thread::hardware_concurrency(), 4096, GLOBAL_ALLOC, "meshopt");
    for (size_t mi = 0, i = 0; i < data->meshes_count; i++)
    {
        auto& mesh = data->meshes[i];
        auto& [mmin, mmax] = submeshIndices.emplace_back(mi, mi);
        for (size_t p = 0; p < mesh.primitives_count; p++)
        {
            auto* sub = mesh.primitives + p;
            CHECK(sub->type == cgltf_primitive_type_triangles);
            // cgltf does not guarantee thread-safety with its accessors.
            // Load first - otherwise UB as I've seen from plenty of examples
            outMeshes[mi] = SceneLoadGLTFSubmesh(sub);
            pool.Push(
                [&](size_t index)
                {
                    auto& submesh = outMeshes[index];
                    LOG(Meshopt, LogInfo, "-- Optimizing {}, vtx: {}, idx: {}", index, submesh.vertices.size(),
                        submesh.lods[0].indices.size());
                    submesh.Optimize();
                    submesh.ClusterizeDAG();
                    submesh.Quantize();
                    LOG(Meshopt, LogInfo, "-- Completed {}", index);
                },
                mi++);
        }
        mmax = mi;
    }
    /* Instances */
    for (size_t i = 0; i < data->nodes_count; i++)
    {
        const cgltf_node* node = &data->nodes[i];
        auto getTransform = [&](FTransform& outTransform) -> void
        {
            mat4 world;
            cgltf_node_transform_world(node, reinterpret_cast<float*>(&world));
            decompose(world, outTransform.scale, outTransform.rotation, outTransform.transform);
        };
        if (node->mesh)
        {
            FInstance instance{};
            getTransform(instance.transform);
            auto meshIndex = cgltf_mesh_index(data, node->mesh);
            auto [mmin, mmax] = submeshIndices[meshIndex];
            for (size_t j = mmin; j < mmax; j++)
            {
                instance.meshIndex = j;
                outInstances.emplace_back(instance);
            }
        }
        if (node->camera)
        {
            auto& camera = outCameras.emplace_back();
            getTransform(camera.transform);
            camera.fovY = node->camera->data.perspective.yfov;
        }
    }
    pool.Join();
    LOG(LoadGLTF, LogInfo, "Scene load complete");
}

const uint32_t kSceneMagic = fourCC("FSCN");
void FSerialize(FWriter& w, Vector<FMesh> const& meshes, Vector<FInstance> const& instances,
                Vector<FCamera> const& cameras)
{
    FSerialize(w, kSceneMagic);
    FSerialize(w, meshes);
    FSerialize(w, instances);
    FSerialize(w, cameras);
}
void FDeserialize(FReader& r, Vector<FMesh>& meshes, Vector<FInstance>& instances, Vector<FCamera>& cameras)
{
    uint32_t magic;
    FDeserialize(r, magic);
    CHECK_MSG(magic == kSceneMagic, "Bad magic. Expected {}, got {}", kSceneMagic, magic);
    FDeserialize(r, meshes);
    FDeserialize(r, instances);
    FDeserialize(r, cameras);
}
void SceneLoadFromFile(StringView scenePath, Vector<FMesh>& outMeshes, Vector<FInstance>& outInstances,
                       Vector<FCamera>& outCameras)
{
    auto ext = std::filesystem::path(scenePath.data()).extension().string();
    if (ext == ".gltf" || ext == ".glb")
    {
        SceneLoadGLTF(scenePath, outMeshes, outInstances, outCameras);
    }
    else
    {
        FileReader reader(scenePath);
        FDeserialize(reader, outMeshes, outInstances, outCameras);
    }
}
void SceneSaveBinFile(StringView path, Vector<FMesh> const& meshes, Vector<FInstance> const& instances,
                  Vector<FCamera> const& cameras)
{
    FileWriter writer(path);
    FSerialize(writer, meshes, instances, cameras);
}
