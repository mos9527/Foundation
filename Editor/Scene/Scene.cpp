#define CGLTF_IMPLEMENTATION
#define CGLTF_VALIDATE_ENABLE_ASSERTS 1
#include "Scene.hpp"
#include <Math/Decompose.hpp>
#include <algorithm>
#include <cgltf.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numbers>
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
            cgltf_accessor_unpack_floats(acc, unpack.data(), numVertices * 2); // VEC2
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

// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#images
Optional<FTexture2D> loadGLTFTexture(cgltf_texture* texture, StringView scenePath, bool gamma = false)
{
    if (texture->image)
    {
        if (auto* buf = texture->image->buffer_view)
        {
            FTexture2D res(GLOBAL_ALLOC);
            Span<const unsigned char> imgData = {static_cast<const unsigned char*>(buf->buffer->data) + buf->offset, buf->size};
            return LoadRGBA8(res, imgData, gamma), res;
        }
        // cgltf only decodes percent-encoding on paths it opens itself (buffer files);
        // image URIs in cgltf_data are left as-is, so do it ourselves before touching the FS.
        String uri = texture->image->uri;
        uri.resize(cgltf_decode_uri(uri.data()));
        String imageNameWE = std::filesystem::path(uri).stem().string();
        std::filesystem::path dir = std::filesystem::path(scenePath.data()).parent_path();
        dir = dir / std::filesystem::path(uri).parent_path();
        // Try common extensions
        const char* extensions[] = {".png", ".jpg", ".jpeg", ".bmp"};
        for (auto ext : extensions)
        {
            auto imagePath = dir / (imageNameWE + ext);
            if (std::filesystem::exists(imagePath))
            {
                FTexture2D res(GLOBAL_ALLOC);
                return LoadRGBA8(res, imagePath.string(), gamma), res;
            }
        }
        // DDS?
        {
            auto imagePath = dir / (imageNameWE + ".dds");
            if (std::filesystem::exists(imagePath))
            {
                FTexture2D res(GLOBAL_ALLOC);
                return LoadDDS(res, imagePath.string()), res;
            }
        }
        LOG(Scene, LogWarn, "Texture image file not found: {}", uri);
        return {};
    }
    return {};
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
    /* Materials */
    // NOTE: Material 0 is reserved as the default material:
    // https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#default-material
    scene.mMaterials.clear();
    scene.mMaterials.emplace_back(FMaterial{
        .baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f},
    });
    // Extra texture flags. Mostly used for sRGB to linear conversion
    constexpr unsigned kTextureInSRGB = 1 << 0;
    Vector<unsigned> textureFlags(data->textures_count, 0, GLOBAL_ALLOC);
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
            {
                size_t index = cgltf_texture_index(data, mat->pbr_metallic_roughness.base_color_texture.texture);
                textureFlags[index] |= kTextureInSRGB;
                material.baseColorTexture = static_cast<uint32_t>(index);
            }
            if (mat->pbr_metallic_roughness.metallic_roughness_texture.texture)
                material.metallicRoughnessTexture =
                    static_cast<uint32_t>(cgltf_texture_index(data, mat->pbr_metallic_roughness.metallic_roughness_texture.texture));
        }
        if (mat->has_pbr_specular_glossiness)
        {
            const auto& sg = mat->pbr_specular_glossiness;
            // Diffuse factor → base color factor
            material.baseColorFactor = {sg.diffuse_factor[0], sg.diffuse_factor[1], sg.diffuse_factor[2], sg.diffuse_factor[3]};
            // Glossiness → roughness
            material.roughnessFactor = 1.0f - sg.glossiness_factor;
            // Approximate metallic from specular luminance (perceived brightness)
            float specLuminance = 0.2126f * sg.specular_factor[0] + 0.7152f * sg.specular_factor[1] + 0.0722f * sg.specular_factor[2];
            // Dielectric F0 ≈ 0.04; anything significantly above that is metallic
            constexpr float kDielectricF0 = 0.04f;
            material.metallicFactor = std::clamp((specLuminance - kDielectricF0) / (1.0f - kDielectricF0), 0.0f, 1.0f);
            // Diffuse texture → base color texture (sRGB)
            if (sg.diffuse_texture.texture)
            {
                size_t index = cgltf_texture_index(data, sg.diffuse_texture.texture);
                textureFlags[index] |= kTextureInSRGB;
                material.baseColorTexture = static_cast<uint32_t>(index);
            }
            // NOTE: specular_glossiness_texture is not mapped — no separate metallic/roughness texture slot for it
        }
        if (mat->normal_texture.texture)
            material.normalTexture = static_cast<uint32_t>(cgltf_texture_index(data, mat->normal_texture.texture));
        if (mat->emissive_texture.texture)
            material.emissiveTexture = static_cast<uint32_t>(cgltf_texture_index(data, mat->emissive_texture.texture));
        material.emissiveFactor = {mat->emissive_factor[0], mat->emissive_factor[1], mat->emissive_factor[2]};
        material.emissiveFactor *= mat->emissive_strength.emissive_strength;
        material.transmissionFactor = mat->transmission.transmission_factor;
        material.ior = mat->has_ior ? mat->ior.ior : 1.5f;
        material.subsurfaceFactor = 0.0f;
        material.subsurfaceColor = {1.0f, 1.0f, 1.0f};
        material.subsurfaceRadius = {1.0f, 1.0f, 1.0f};
        if (mat->has_subsurface)
        {
            material.subsurfaceFactor = std::clamp(mat->subsurface.subsurface_weight, 0.0f, 1.0f);
            material.subsurfaceRadius = {
                mat->subsurface.subsurface_radius[0] * mat->subsurface.subsurface_scale,
                mat->subsurface.subsurface_radius[1] * mat->subsurface.subsurface_scale,
                mat->subsurface.subsurface_radius[2] * mat->subsurface.subsurface_scale
            };
        }
        scene.mMaterials.emplace_back(material);
    }
    /* Textures and Meshes */
    size_t numSubmeshes = 0;
    for (size_t i = 0; i < data->meshes_count; i++)
        numSubmeshes += data->meshes[i].primitives_count;
    scene.mTextures.resize(data->textures_count, GLOBAL_ALLOC);
    size_t jobCount = data->textures_count + numSubmeshes;
    ThreadPool pool(std::thread::hardware_concurrency(), ThreadPool::getTaskSize(jobCount), GLOBAL_ALLOC);
    for (size_t i = 0; i < data->textures_count; i++)
        pool.Push(
            [&](cgltf_texture* src, FTexture2D* dst, StringView basePath)
            {
                size_t index = cgltf_texture_index(data, src);
                String name = src->name ? src->name : fmt::format("{}_{}", basePath, index);
                unsigned flags = textureFlags[index];
                LOG(Scene, LogInfo, "Loading texture {}", name);
                auto loaded = loadGLTFTexture(src, basePath, flags & kTextureInSRGB);
                if (loaded.has_value())
                {
                    // Raw image. Compress to BC7 if needed
                    if (loaded->GetFormat() == RHIResourceFormat::R8G8B8A8Unorm ||
                        loaded->GetFormat() == RHIResourceFormat::R8G8B8A8Srgb)
                    {
                        loaded->GenerateMips();
                        *dst = loaded->EncodeBC7();
                    }
                    else
                        *dst = loaded.value();
                    LOG(Scene, LogInfo, "Loaded texture {}", name);
                }
                else
                {
                    LOG(Scene, LogWarn, "No texture loaded for {}", name);
                }
            },
            &data->textures[i], &scene.mTextures[i], path);
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
    /* Instances / Cameras / Light */
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
            if (node->camera->has_lens)
            {
                camera.lensEnabled = true;
                camera.sensorHeightMm = node->camera->lens.sensor_size;
                camera.fStop = node->camera->lens.fstop;
                camera.focusDistance = node->camera->lens.focus_distance;
            }
        }
        if (node->light)
        {
            auto& light = scene.mLights.emplace_back();
            getTransform(light.transform);
            light.color = float3{node->light->color[0], node->light->color[1], node->light->color[2]};
            light.power = node->light->intensity / 683.0f; // Convert from lm to W for white light
            light.range = node->light->range; // 0 = infinite
            // Map glTF light type
            switch (node->light->type)
            {
            case cgltf_light_type_directional:
                light.type = FLightType::Directional;
                break;
            case cgltf_light_type_point:
                light.type = FLightType::Point;
                break;
            case cgltf_light_type_spot:
                light.type = FLightType::Spot;
                light.spotInnerConeAngle = node->light->spot_inner_cone_angle;
                light.spotOuterConeAngle = node->light->spot_outer_cone_angle;
                break;
            default:
                light.type = FLightType::Directional;
                break;
            }
        }
        // EXT_lights_area: rectangle / disk area lights.
        // https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Vendor/EXT_lights_area
        if (node->light_area)
        {
            auto& light = scene.mLights.emplace_back();
            getTransform(light.transform);
            // "world scale" per spec: largest absolute component of node's world scale.
            float ws = std::max({std::abs(light.transform.scale.x), std::abs(light.transform.scale.y),
                                 std::abs(light.transform.scale.z)});
            // Fold world-scale into half-extents so downstream (FLightToGSLight) can ignore transform.scale,
            // which matches how punctual lights are handled.
            light.transform.scale = float3{1, 1, 1};

            const cgltf_light_area* la = node->light_area;
            light.color = float3{la->color[0], la->color[1], la->color[2]};
            light.range = 0.0f; // EXT_lights_area has no range
            light.twoSided = false; // Spec: emits from one side (-Z local)
            light.normalize = true; // Treat `power` below as total flux; FLightToGSLight divides by area*pi → nits

            // glTF size → full width/height; FLight stores *half-extents*.
            // rect:  width = size * aspect, height = size
            // disk:  diameter = size  →  radius = size / 2 on both axes
            float fullW = 1.0f, fullH = 1.0f;
            if (la->type == cgltf_light_area_type_disk)
            {
                light.type = FLightType::Disk;
                fullW = fullH = la->size * ws;
            }
            else // rect (and default)
            {
                light.type = FLightType::Rect;
                fullW = la->size * la->rect_aspect * ws;
                fullH = la->size * ws;
            }
            light.width = fullW * 0.5f;
            light.height = fullH * 0.5f;

            // Defined for unit areas
            light.power = la->intensity;
        }
    }
    pool.Join();
}
void LoadFSCN(StringView path, FScene& scene)
{
    FileReader reader(path);
    FDeserialize(reader, scene);
}
void LoadScene(StringView path, FScene& scene)
{
    auto ext = std::filesystem::path(path.data()).extension().string();
    if (ext == ".fscn")
        LoadFSCN(path, scene);
    else
        LoadGLTF(path, scene);
}
