#define CGLTF_IMPLEMENTATION
#define CGLTF_VALIDATE_ENABLE_ASSERTS 1
#include "Scene.hpp"
#include <Math/Decompose.hpp>
#include <cgltf.h>
#include <filesystem>
#include <fstream>
#include "Mesh.hpp"
// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#meshes
FMesh loadGLTFSubmesh(cgltf_primitive* submesh)
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

FTexture2D createNullTexture()
{
    // 2x2 black/magenta checkerboard
    const Array<uint32_t, 4> data{0xFF000000, 0xFFFF00FF, 0xFFFF00FF, 0xFF000000};
    FTexture2D texture(GLOBAL_ALLOC);
    ddsCreateHeader(texture.header, 2, 2, 1);
    ddsSetFormat(texture.header, texture.header10, 1, RHIResourceFormat::B8G8R8A8Unrom);
    texture.data.assign(data.begin(), data.end());
    return texture;
}

// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#images
FTexture2D loadGLTFTexture(cgltf_texture* texture, StringView scenePath)
{
    if (texture->image)
    {
        if (auto* buf = texture->image->buffer_view)
        {
            FTexture2D res(GLOBAL_ALLOC);
            Span imgData = {static_cast<const unsigned char*>(buf->buffer->data) + buf->offset, buf->size};
            return LoadRGBA8(res, imgData, false), res;
        }
        auto imagePath = std::filesystem::path(scenePath.data()).parent_path() / texture->image->uri;
        if (std::filesystem::exists(imagePath))
        {
            FTexture2D res(GLOBAL_ALLOC);
            return LoadRGBA8(res, imagePath.string(), false), res;
        }
        LOG(Scene, LogWarn, "Texture image file not found: {}", imagePath.string());
        return createNullTexture();
    }
    return createNullTexture();
}

void LoadGLTF(StringView path, FScene& scene)
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
    /* Textures and Meshes */
    size_t numSubmeshes = 0;
    for (size_t i = 0; i < data->meshes_count; i++)
        numSubmeshes += data->meshes[i].primitives_count;
    ThreadPool pool(std::thread::hardware_concurrency(), ThreadPool::getTaskSize(data->textures_count + numSubmeshes),
                    GLOBAL_ALLOC);
    scene.mTextures.resize(data->textures_count + 1, GLOBAL_ALLOC);
    // NOTE: 0 is reserved as the null texture
    scene.mTextures[0] = createNullTexture();
    for (size_t i = 0; i < data->textures_count; i++)
        pool.Push(
            [=](cgltf_texture* src, FTexture2D* dst, StringView basePath)
            {
                String name = src->name ? src->name : fmt::format("{}_{}", basePath, src - data->textures);
                LOG(Scene, LogInfo, "Loading texture {}", name);
                *dst = loadGLTFTexture(src, basePath);
                LOG(Scene, LogInfo, "Encoding texture {} to BC7", name);
                *dst = dst->EncodeBC7();
                LOG(Scene, LogInfo, "Loaded texture {}", name);
            },
            &data->textures[i], &scene.mTextures[i + 1], path);
    // Mesh's submesh children
    // These will be flattened later on into @ref FInstance
    Vector<Pair<size_t, size_t>> submeshIndices(GLOBAL_ALLOC);
    scene.mMeshes.resize(scene.mMeshes.size() + numSubmeshes, GLOBAL_ALLOC);
    for (size_t mi = 0, i = 0; i < data->meshes_count; i++)
    {
        auto& mesh = data->meshes[i];
        auto& [mmin, mmax] = submeshIndices.emplace_back(mi, mi);
        for (size_t p = 0; p < mesh.primitives_count; p++)
        {
            auto* sub = mesh.primitives + p;
            CHECK(sub->type == cgltf_primitive_type_triangles);
            scene.mMeshes[mi] = loadGLTFSubmesh(sub);
            pool.Push(
                [&](size_t index)
                {
                    auto& submesh = scene.mMeshes[index];
                    LOG(Meshopt, LogInfo, "Optimizing submesh {}, vtx: {}, idx: {}", index, submesh.vertices.size(),
                        submesh.lods[0].indices.size());
                    submesh.Optimize();
                    submesh.ClusterizeDAG();
                    submesh.Quantize();
                    LOG(Meshopt, LogInfo, "Optimized {}", index);
                },
                mi++);
        }
        mmax = mi;
    }
    /* Materials */
    // NOTE: Material 0 is reserved as the default material:
    // https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#default-material
    scene.mMaterials.clear();
    scene.mMaterials.emplace_back();
    for (size_t i = 0; i < data->materials_count; i++)
    {
        const cgltf_material* mat = &data->materials[i];
        FMaterial material{};
        if (mat->has_pbr_metallic_roughness)
        {
            material.baseColorFactor = {
                mat->pbr_metallic_roughness.base_color_factor[0], mat->pbr_metallic_roughness.base_color_factor[1],
                mat->pbr_metallic_roughness.base_color_factor[2], mat->pbr_metallic_roughness.base_color_factor[3]};
            material.metallicFactor = mat->pbr_metallic_roughness.metallic_factor;
            material.roughnessFactor = mat->pbr_metallic_roughness.roughness_factor;
            if (mat->pbr_metallic_roughness.base_color_texture.texture)
                material.baseColorTexture =
                    cgltf_texture_index(data, mat->pbr_metallic_roughness.base_color_texture.texture) + 1u;
            if (mat->pbr_metallic_roughness.metallic_roughness_texture.texture)
                material.metallicRoughnessTexture =
                    cgltf_texture_index(data, mat->pbr_metallic_roughness.metallic_roughness_texture.texture) + 1u;
        }
        if (mat->normal_texture.texture)
            material.normalTexture = cgltf_texture_index(data, mat->normal_texture.texture) + 1u;
        if (mat->emissive_texture.texture)
            material.emissiveTexture = cgltf_texture_index(data, mat->emissive_texture.texture) + 1u;
        material.emissiveFactor = {mat->emissive_factor[0], mat->emissive_factor[1], mat->emissive_factor[2]};
        scene.mMaterials.emplace_back(material);
    }
    /* Instances & Cameras */
    scene.mInstances.clear();
    scene.mCameras.clear();
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
                auto* sub = node->mesh->primitives + j - mmin;
                instance.meshIndex = j;
                if (sub->material)
                    instance.materialIndex = cgltf_material_index(data, sub->material) + 1u;
                scene.mInstances.emplace_back(instance);
            }
        }
        if (node->camera)
        {
            auto& camera = scene.mCameras.emplace_back();
            getTransform(camera.transform);
            camera.fovY = node->camera->data.perspective.yfov;
        }
    }
    pool.Join();
}
