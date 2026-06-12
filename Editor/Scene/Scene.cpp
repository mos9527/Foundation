#define CGLTF_IMPLEMENTATION
#define CGLTF_VALIDATE_ENABLE_ASSERTS 1
#include "Scene.hpp"
#include <Core/ThreadPool.hpp>
#include <Math/Decompose.hpp>
#include <algorithm>
#include <cctype>
#include <cgltf.h>
#include <climits>
#include <cstring>
#include <filesystem>
#include <lz4.h>
#include <numeric>
#include <string_view>
#include <type_traits>
#include <RHICore/Device.hpp>
#include <Renderer/Postprocess.hpp>
#include <Renderer/GPUScene.hpp>
#include <Renderer/Mesh.hpp>
#include <Renderer/Animation.hpp>
#include "Curve.hpp"

namespace
{
String DecodeURI(std::string_view encoded)
{
    String uri(encoded);
    uri.resize(cgltf_decode_uri(uri.data()));
    return uri;
}

String LowerExtension(std::filesystem::path const& path)
{
    String ext = path.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

std::string_view Trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

bool ParseLUTTuple(std::string_view tuple, std::string_view expectedKind,
                          std::string_view& outView, std::string_view& outLook)
{
    size_t first = tuple.find(" / ");
    if (first == std::string_view::npos)
        return false;
    size_t second = tuple.find(" / ", first + 3);
    if (second == std::string_view::npos)
        return false;

    std::string_view kind = Trim(tuple.substr(0, first));
    if (kind != expectedKind)
        return false;

    outView = Trim(tuple.substr(first + 3, second - (first + 3)));
    outLook = Trim(tuple.substr(second + 3));
    return !outView.empty() && !outLook.empty();
}

void LoadFoundationColorManagementExtension(cgltf_data const* data, FSceneGlobals& result)
{
    if (!data->has_foundation_color_management)
        return;

    cgltf_foundation_color_management const& colorManagement = data->foundation_color_management;
    if (colorManagement.has_post_exposure)
        result.postExposure = colorManagement.post_exposure;

    std::string_view view;
    std::string_view look;
    if (colorManagement.sdr && ParseLUTTuple(colorManagement.sdr, "SDR", view, look))
        result.viewLutSdrIndex = Postprocess::MatchViewLUTIndex(Postprocess::ViewLUTDomain::SDR, view, look,
                                                                Postprocess::GetDefaultViewLUTIndex(Postprocess::ViewLUTDomain::SDR));
    if (colorManagement.hdr && ParseLUTTuple(colorManagement.hdr, "HDR", view, look))
        result.viewLutHdrIndex = Postprocess::MatchViewLUTIndex(Postprocess::ViewLUTDomain::HDR, view, look,
                                                                Postprocess::GetDefaultViewLUTIndex(Postprocess::ViewLUTDomain::HDR));
}

Optional<String> LoadFoundationEnvironmentExtension(cgltf_data const* data, StringView scenePath, FLight& environmentLight)
{
    cgltf_scene const* gltfScene = data->scene ? data->scene : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (!gltfScene || !gltfScene->has_foundation_environment)
        return {};

    cgltf_foundation_environment const& environment = gltfScene->foundation_environment;
    if (environment.type == cgltf_foundation_environment_type_color)
    {
        environmentLight = MakeDefaultEnvironmentLight();
        environmentLight.color = {environment.color[0], environment.color[1], environment.color[2]};
        environmentLight.power = environment.strength;
        environmentLight.environmentMap = false;
        return {};
    }

    if (environment.type == cgltf_foundation_environment_type_hdri)
    {
        CHECK_MSG(environment.projection == cgltf_foundation_environment_projection_longlat,
                  "EXT_foundation_environment supports only longlat/equirectangular HDRI projection");
        CHECK_MSG(environment.uri, "EXT_foundation_environment HDRI requires uri");

        String uri = DecodeURI(environment.uri);
        std::filesystem::path hdriPath = std::filesystem::path(scenePath.data()).parent_path() / uri;
        environmentLight = MakeDefaultEnvironmentLight();
        environmentLight.power = environment.strength;
        environmentLight.environmentMap = true;
        environmentLight.environmentAzimuthOffset = environment.azimuth_offset;
        return hdriPath.string();
    }

    CHECK_MSG(false, "EXT_foundation_environment has unsupported type");
    return {};
}

void LoadFoundationMaterialExtension(cgltf_material const* src, FMaterial& material)
{
    if (!src->has_foundation_materials)
        return;

    cgltf_foundation_materials const& foundationMaterials = src->foundation_materials;
    if (foundationMaterials.has_shader_block)
    {
        switch (foundationMaterials.shader_block)
        {
        case cgltf_foundation_material_shader_block_principled:
            material.shaderBlockID = FMaterialShaderBlock::Principled;
            break;
        case cgltf_foundation_material_shader_block_hair:
            material.shaderBlockID = FMaterialShaderBlock::Hair;
            break;
        default:
            CHECK_MSG(false, "EXT_foundation_materials has unsupported shaderBlock");
            break;
        }
    }

    if (material.shaderBlockID == FMaterialShaderBlock::Hair)
    {
        CHECK_MSG(foundationMaterials.hair_model == cgltf_foundation_material_hair_model_chiang,
                  "EXT_foundation_materials hair supports only model 'chiang'");

        if (foundationMaterials.has_hair_beta_m)
            material.hairBetaM = foundationMaterials.hair_beta_m;
        if (foundationMaterials.has_hair_beta_n)
            material.hairBetaN = foundationMaterials.hair_beta_n;
        if (foundationMaterials.has_hair_alpha)
            material.hairAlpha = foundationMaterials.hair_alpha;
        if (foundationMaterials.has_ior)
            material.ior = foundationMaterials.ior;
    }
}

void ValidateSceneHeader(FSceneHeader const& header)
{
    CHECK_MSG(header.magic == kSceneMagic, "Unsupported FSCN magic");
    CHECK_MSG(header.headerSize == sizeof(FSceneHeader), "Unsupported FScene header size");
    CHECK_MSG(header.metadataSize <= SIZE_MAX, "FScene metadata too large for this platform");
    CHECK_MSG(header.payloadAlignment != 0, "FScene payload alignment must be non-zero");
    CHECK_MSG((header.payloadAlignment & (header.payloadAlignment - 1u)) == 0,
              "FScene payload alignment must be a power of two");
    CHECK_MSG(header.payloadOffset >= sizeof(FSceneHeader), "Unsupported FScene payload offset");
    CHECK_MSG(header.payloadOffset % header.payloadAlignment == 0, "Misaligned FScene payload offset");
    CHECK_MSG(header.metadataOffset >= header.payloadOffset, "Unsupported FScene metadata offset");
    CHECK_MSG(header.metadataOffset % 16 == 0, "Misaligned FScene metadata offset");
    CHECK_MSG(header.metadataOffset <= header.fileSize, "FScene metadata offset exceeds file size");
    CHECK_MSG(header.metadataSize <= header.fileSize - header.metadataOffset, "FScene metadata exceeds file size");
    CHECK_MSG(header.version == kSceneVersion, "Unsupported FScene version {}", header.version);
}

void ValidateBlobRef(FSceneHeader const& header, FBlobRef const& blob, const char* name)
{
    CHECK_MSG(blob.codec == FBlobCodec::None || blob.codec == FBlobCodec::LZ4,
              "{} has unsupported blob codec {}", name, static_cast<uint32_t>(blob.codec));
    CHECK_MSG(blob.count == 0 || blob.stride != 0, "{} has zero stride with non-zero count", name);
    CHECK_MSG(blob.decodedSize == uint64_t(blob.count) * blob.stride,
              "{} decoded size mismatch: {} bytes for {} elements with stride {}",
              name, blob.decodedSize, blob.count, blob.stride);
    CHECK_MSG(blob.decodedSize <= SIZE_MAX, "{} is too large for this platform", name);
    if (blob.decodedSize == 0)
    {
        CHECK_MSG(blob.storedSize == 0, "{} stores bytes for an empty blob", name);
        return;
    }

    CHECK_MSG(blob.storedSize != 0, "{} has no stored bytes", name);
    switch (blob.codec)
    {
    case FBlobCodec::None:
        CHECK_MSG(blob.storedSize == blob.decodedSize, "{} uncompressed blob size mismatch", name);
        break;
    case FBlobCodec::LZ4:
        CHECK_MSG(blob.storedSize <= blob.decodedSize, "{} compressed blob is larger than decoded data", name);
        break;
    default:
        break;
    }

    uint64_t payloadSize = header.metadataOffset - header.payloadOffset;
    CHECK_MSG(blob.offset <= payloadSize, "{} offset exceeds payload size", name);
    CHECK_MSG(blob.storedSize <= payloadSize - blob.offset, "{} exceeds payload bounds", name);
}

template <typename T>
void ValidateBlobArray(FSceneHeader const& header, FBlobRef const& blob, const char* name)
{
    static_assert(std::is_trivially_copyable_v<T>);
    ValidateBlobRef(header, blob, name);
    CHECK_MSG(blob.stride == sizeof(T), "{} stride mismatch: expected {} got {}", name, sizeof(T), blob.stride);
    CHECK_MSG(blob.count <= SIZE_MAX / sizeof(T), "{} count is too large for this platform", name);
}

void ValidateTextureIndex(uint32_t index, size_t textureCount, const char* name)
{
    if (index != kInvalidTexture)
        CHECK_MSG(index < textureCount, "{} references texture {} but only {} textures exist", name, index, textureCount);
}

void ValidateSceneTables(FSceneHeader const& header, FSceneTables const& tables)
{
    CHECK_MSG(tables.cameras.size() <= UINT32_MAX, "FScene camera table is too large");
    CHECK_MSG(tables.lights.size() <= UINT32_MAX, "FScene light table is too large");
    CHECK_MSG(tables.instances.size() <= UINT32_MAX, "FScene instance table is too large");
    CHECK_MSG(tables.materials.size() <= UINT32_MAX, "FScene material table is too large");
    CHECK_MSG(tables.meshes.size() <= UINT32_MAX, "FScene mesh table is too large");
    CHECK_MSG(tables.curves.size() <= UINT32_MAX, "FScene curve table is too large");
    CHECK_MSG(tables.textures.size() <= UINT32_MAX, "FScene texture table is too large");

    size_t environmentLightCount = 0;
    for (auto const& light : tables.lights)
    {
        switch (light.type)
        {
        case FLightType::Directional:
        case FLightType::Point:
        case FLightType::Spot:
        case FLightType::Disk:
        case FLightType::Rect:
            break;
        case FLightType::Environment:
            environmentLightCount++;
            if (light.environmentMap)
            {
                CHECK_MSG(light.environmentTexture != kInvalidTexture,
                          "FScene EnvMap environment light requires environmentTexture");
                ValidateTextureIndex(light.environmentTexture, tables.textures.size(), "light.environmentTexture");
            }
            break;
        default:
            CHECK_MSG(false, "FScene light has unsupported type {}", static_cast<uint32_t>(light.type));
            break;
        }
    }
    CHECK_MSG(environmentLightCount == 1, "FScene must have exactly one environment light, got {}",
              environmentLightCount);
    CHECK_MSG(!tables.lights.empty() && tables.lights.front().type == FLightType::Environment,
              "FScene environment light must be the first light");

    for (auto const& material : tables.materials)
    {
        switch (material.shaderBlockID)
        {
        case FMaterialShaderBlock::Principled:
        case FMaterialShaderBlock::Hair:
            break;
        default:
            CHECK_MSG(false, "FScene material has unsupported shader block {}",
                      static_cast<uint32_t>(material.shaderBlockID));
            break;
        }
        ValidateTextureIndex(material.baseColorTexture, tables.textures.size(), "material.baseColorTexture");
        ValidateTextureIndex(material.emissiveTexture, tables.textures.size(), "material.emissiveTexture");
        ValidateTextureIndex(material.metallicRoughnessTexture, tables.textures.size(), "material.metallicRoughnessTexture");
        ValidateTextureIndex(material.normalTexture, tables.textures.size(), "material.normalTexture");
        ValidateTextureIndex(material.transmissionTexture, tables.textures.size(), "material.transmissionTexture");
        ValidateTextureIndex(material.specularTexture, tables.textures.size(), "material.specularTexture");
        ValidateTextureIndex(material.specularColorTexture, tables.textures.size(), "material.specularColorTexture");
        ValidateTextureIndex(material.anisotropyTexture, tables.textures.size(), "material.anisotropyTexture");
        ValidateTextureIndex(material.clearcoatTexture, tables.textures.size(), "material.clearcoatTexture");
        ValidateTextureIndex(material.clearcoatRoughnessTexture, tables.textures.size(), "material.clearcoatRoughnessTexture");
    }

    for (auto const& instance : tables.instances)
    {
        CHECK_MSG(instance.materialIndex < tables.materials.size(), "FScene instance material index out of range");
        switch (instance.type)
        {
        case FInstanceType::Mesh:
            CHECK_MSG(instance.resourceIndex < tables.meshes.size(), "FScene instance mesh index out of range");
            break;
        case FInstanceType::Curve:
            CHECK_MSG(instance.resourceIndex < tables.curves.size(), "FScene instance curve index out of range");
            break;
        default:
            CHECK_MSG(false, "FScene instance has unsupported type {}", static_cast<uint32_t>(instance.type));
            break;
        }
    }

    for (auto const& mesh : tables.meshes)
    {
        CHECK_MSG(!mesh.lods.empty(), "FScene mesh has no LODs");
        ValidateBlobArray<FQVertex>(header, mesh.vertices, "mesh.vertices");
        CHECK_MSG(mesh.vertexCount == mesh.vertices.count, "FScene mesh vertex count mismatch");
        for (auto const& lod : mesh.lods)
        {
            ValidateBlobArray<uint32_t>(header, lod.indices, "mesh.lod.indices");
            CHECK_MSG(lod.indexCount == lod.indices.count, "FScene mesh LOD index count mismatch");
            CHECK_MSG(lod.indexCount % 3 == 0, "FScene mesh LOD index count must be triangle-aligned");
        }
        ValidateBlobArray<FLODGroup>(header, mesh.dagGroups, "mesh.dagGroups");
        ValidateBlobArray<FMeshlet>(header, mesh.dagMeshlets, "mesh.dagMeshlets");
        ValidateBlobArray<uint8_t>(header, mesh.dagMeshletTri, "mesh.dagMeshletTri");
        ValidateBlobArray<uint32_t>(header, mesh.dagMeshletVtx, "mesh.dagMeshletVtx");
        if (mesh.skinBinding.count != 0)
        {
            ValidateBlobArray<FSkinBinding>(header, mesh.skinBinding, "mesh.skinBinding");
            CHECK_MSG(mesh.skinBinding.count == mesh.vertexCount, "FScene mesh skin binding count mismatch");
            CHECK_MSG(mesh.skeleton >= 0 && static_cast<size_t>(mesh.skeleton) < tables.skeletons.size(),
                      "FScene mesh skeleton index out of range");
        }
        if (mesh.morphTargetCount != 0)
        {
            ValidateBlobArray<float3>(header, mesh.morphPositions, "mesh.morphPositions");
            CHECK_MSG(mesh.morphPositions.count == mesh.morphTargetCount * mesh.vertexCount,
                      "FScene mesh morph delta count mismatch");
        }
        CHECK_MSG(mesh.morphTrack == -1 || static_cast<size_t>(mesh.morphTrack) < tables.morphTracks.size(),
                  "FScene mesh morph track index out of range");
    }

    for (auto const& clip : tables.clips)
    {
        CHECK_MSG(clip.skeleton >= 0 && static_cast<size_t>(clip.skeleton) < tables.skeletons.size(),
                  "FScene clip skeleton index out of range");
        uint32_t jointCount = tables.skeletons[clip.skeleton].Count();
        for (auto const& channel : clip.channels)
            CHECK_MSG(channel.joint < jointCount, "FScene clip channel joint index out of range");
    }

    CHECK_MSG(tables.sceneNodeSkeleton == -1 ||
                  static_cast<size_t>(tables.sceneNodeSkeleton) < tables.skeletons.size(),
              "FScene scene-node skeleton index out of range");
    uint32_t const sceneNodeCount =
        tables.sceneNodeSkeleton >= 0 ? tables.skeletons[tables.sceneNodeSkeleton].Count() : 0u;
    auto validateNode = [&](int32_t node, const char* what)
    {
        CHECK_MSG(node == -1 || (tables.sceneNodeSkeleton >= 0 && static_cast<uint32_t>(node) < sceneNodeCount),
                  "FScene {} node index out of range", what);
    };
    for (auto const& instance : tables.instances)
        validateNode(instance.node, "instance");
    for (auto const& light : tables.lights)
        validateNode(light.node, "light");
    for (auto const& camera : tables.cameras)
        validateNode(camera.node, "camera");

    for (auto const& curve : tables.curves)
    {
        ValidateBlobArray<FCurvePoint>(header, curve.points, "curve.points");
        ValidateBlobArray<FSerializedCurveSegment>(header, curve.segments, "curve.segments");
        ValidateBlobArray<FSerializedCurveAABB>(header, curve.aabbs, "curve.aabbs");
        CHECK_MSG(curve.segments.count == curve.aabbs.count, "FScene curve AABB count mismatch");
        CHECK_MSG(curve.materialIndex < tables.materials.size(), "FScene curve material index out of range");
    }

    for (auto const& texture : tables.textures)
    {
        if (!static_cast<FTextureHeader const&>(texture).IsValid())
        {
            CHECK_MSG(texture.subresources.empty(), "Invalid FScene texture must not carry subresource blobs");
            continue;
        }

        CHECK_MSG(texture.subresources.size() == texture.GetSubresourceCount(),
                  "FScene texture subresource count mismatch: {} blobs for {} expected subresources",
                  texture.subresources.size(), texture.GetSubresourceCount());
        for (uint32_t layer = 0; layer < texture.GetNumLayers(); ++layer)
        {
            for (uint32_t mip = 0; mip < texture.GetNumMips(); ++mip)
            {
                FBlobRef const& blob = texture.GetSubresourceBlob(layer, mip);
                ValidateBlobArray<unsigned char>(header, blob, "texture.subresources");
                size_t const expectedSize = texture.GetSubresourceSize(layer, mip);
                CHECK_MSG(blob.decodedSize == expectedSize,
                          "FScene texture subresource blob size mismatch: layer {}, mip {}, blob {}, expected {}",
                          layer, mip, blob.decodedSize, expectedSize);
            }
        }
    }
}
}

// https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#meshes
FImportedMesh LoadGLTFSubmesh(const cgltf_primitive* submesh, Allocator* scratchAlloc,
                              const Vector<uint16_t>* jointRemap = nullptr, bool readMorph = false)
{
    CHECK(submesh->type == cgltf_primitive_type_triangles);
    CHECK(scratchAlloc != nullptr);
    FImportedMesh mesh(scratchAlloc);
    // Vertex count would be the same across POSITION, NORMAL, etc. Getting any of those would be enough.
    // Worst storage case is VEC4
    {
        size_t numVertices = submesh->attributes[0].data->count;
        Vector<float> unpack(numVertices * 4, scratchAlloc);
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
    auto& m0 = mesh.lods[0];
    if (submesh->indices)
    {
        size_t numIndices = submesh->indices->count;
        m0.indices.resize(numIndices);
        cgltf_accessor_unpack_indices(submesh->indices, m0.indices.data(), sizeof(uint32_t), numIndices);
    }
    else
    {
        // glTF 2.0 allows omitting `indices`, in which case the primitive is drawn as
        // drawArrays(count = vertexCount). Synthesize a trivial 0..N-1 index buffer so the
        // rest of the pipeline (LOD blobs, meshlet builder, FSCN validation) stays uniform.
        size_t numVertices = submesh->attributes[0].data->count;
        CHECK_MSG(numVertices % 3 == 0,
                  "Non-indexed glTF triangle primitive vertex count is not a multiple of 3");
        m0.indices.resize(numVertices);
        std::iota(m0.indices.begin(), m0.indices.end(), 0u);
    }

    // glTF 2.0 marks NORMAL as optional: when absent, the spec says clients should compute flat
    // normals. We instead synthesize smooth, area-weighted per-vertex normals from the indexed
    // triangle list so the rest of the pipeline (TBN packing, meshlet shading) sees a valid frame.
    // Tangents (if any) are kept; they get re-projected onto the new normal during FQVertex::PackTBN.
    if (cgltf_find_accessor(submesh, cgltf_attribute_type_normal, 0) == nullptr)
    {
        RecomputeNormals(Span<FVertex>(mesh.vertices.data(), mesh.vertices.size()),
                         Span<const uint32_t>(m0.indices.data(), m0.indices.size()));
    }


    // joint indices to the skeleton's topological order. Only present when the mesh is skinned.
    const cgltf_accessor* jointsAcc = cgltf_find_accessor(submesh, cgltf_attribute_type_joints, 0);
    const cgltf_accessor* weightsAcc = cgltf_find_accessor(submesh, cgltf_attribute_type_weights, 0);
    if (jointsAcc && weightsAcc)
    {
        size_t numVertices = submesh->attributes[0].data->count;
        mesh.skin.resize(numVertices);
        for (size_t i = 0; i < numVertices; i++)
        {
            cgltf_uint j[4] = {0, 0, 0, 0};
            float w[4] = {0, 0, 0, 0};
            cgltf_accessor_read_uint(jointsAcc, i, j, 4);
            cgltf_accessor_read_float(weightsAcc, i, w, 4);
            FSkinBinding& bind = mesh.skin[i];
            for (int k = 0; k < 4; k++)
            {
                uint32_t joint = j[k];
                if (jointRemap && joint < jointRemap->size())
                    joint = (*jointRemap)[joint];
                bind.joints[k] = static_cast<uint16_t>(joint);
                bind.weights[k] = w[k];
            }
        }
    }

    // Morph-target POSITION deltas (parallel to vertices), stored target-major. Only read when the
    // mesh is morph-animated (so vertex order stays unoptimized and aligned with the deltas).
    if (readMorph && submesh->targets_count > 0)
    {
        size_t numVertices = submesh->attributes[0].data->count;
        uint32_t targetCount = static_cast<uint32_t>(submesh->targets_count);
        mesh.morphTargetCount = targetCount;
        mesh.morphPositions.assign(static_cast<size_t>(targetCount) * numVertices, float3(0.0f));
        Vector<float> unpack(numVertices * 3, scratchAlloc);
        for (uint32_t t = 0; t < targetCount; t++)
        {
            const cgltf_morph_target& target = submesh->targets[t];
            const cgltf_accessor* posAcc = nullptr;
            for (size_t a = 0; a < target.attributes_count; a++)
                if (target.attributes[a].type == cgltf_attribute_type_position)
                {
                    posAcc = target.attributes[a].data;
                    break;
                }
            if (!posAcc)
                continue;
            cgltf_accessor_unpack_floats(posAcc, unpack.data(), numVertices * 3);
            float3* dst = mesh.morphPositions.data() + static_cast<size_t>(t) * numVertices;
            for (size_t v = 0; v < numVertices; v++)
                dst[v] = float3(unpack[v * 3 + 0], unpack[v * 3 + 1], unpack[v * 3 + 2]);
        }
    }
    return mesh;
}

Optional<FTexture> LoadTexture(StringView path, Allocator* scratchAlloc, bool gamma = false)
{
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    String imageNameWE = std::filesystem::path(path).stem().string();
    // Try common extensions
    {
        const char* extensions[] = {".png", ".jpg", ".jpeg", ".bmp"};
        for (auto ext : extensions)
        {
            auto imagePath = dir / (imageNameWE + ext);
            if (std::filesystem::exists(imagePath))
            {
                FTexture res(scratchAlloc);
                return LoadRGBA8(res, imagePath.string(), gamma), res;
            }
        }
    }
    // DDS?
    {
        auto imagePath = dir / (imageNameWE + ".dds");
        if (std::filesystem::exists(imagePath))
        {
            FTexture res(scratchAlloc);
            return LoadDDS(res, imagePath.string()), res;
        }
    }
    // HDR/HDRI?
    {
        const char* extensions[] = {".hdr", ".hdri"};
        for (auto ext : extensions)
        {
            auto imagePath = dir / (imageNameWE + ext);
            if (std::filesystem::exists(imagePath))
            {
                FTexture res(scratchAlloc);
                return LoadHDR(res, imagePath.string()), res;
            }
        }
    }
    return {};
}
    // https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#images
Optional<FTexture> LoadGLTFTexture(cgltf_texture* texture, StringView scenePath, Allocator* scratchAlloc, bool gamma = false)
{
    CHECK(scratchAlloc != nullptr);
    if (texture->image)
    {
        if (auto* buf = texture->image->buffer_view)
        {
            FTexture res(scratchAlloc);
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
        auto fullpath = dir / imageNameWE;
        auto result = LoadTexture(fullpath.string(), scratchAlloc, gamma);
        if (result)
            return result;
        LOG(Scene, LogWarn, "Texture image file not found: {}", uri);
        return {};
    }
    return {};
}

FCurveBasis LoadGLTFCurveBasis(cgltf_curve_basis basis)
{
    switch (basis)
    {
    case cgltf_curve_basis_bezier:
        return FCurveBasis::Bezier;
    case cgltf_curve_basis_bspline:
        return FCurveBasis::BSpline;
    case cgltf_curve_basis_catmull_rom:
        return FCurveBasis::CatmullRom;
    case cgltf_curve_basis_linear:
    default:
        return FCurveBasis::Linear;
    }
}

void LoadGLTFCurve(const cgltf_data* data, const cgltf_curve* src, FImportedCurve& curve, Allocator* scratchAlloc)
{
    CHECK(scratchAlloc != nullptr);
    CHECK(src->points);
    CHECK(src->curve_vertex_counts);
    CHECK(src->points->type == cgltf_type_vec4);
    CHECK(src->points->component_type == cgltf_component_type_r_32f);
    CHECK(src->curve_vertex_counts->type == cgltf_type_scalar);

    curve.basis = LoadGLTFCurveBasis(src->basis);
    CHECK_MSG(curve.basis == FCurveBasis::Bezier, "EXT_foundation_curves import currently supports only Bezier curves");
    curve.renderMode = FCurveRenderMode::Capsule;
    curve.materialIndex = src->material ? static_cast<uint32_t>(cgltf_material_index(data, src->material) + 1u) : 0u;

    size_t pointCount = src->points->count;
    Vector<float> unpack(pointCount * 4, scratchAlloc);
    cgltf_accessor_unpack_floats(src->points, unpack.data(), unpack.size());
    curve.points.resize(pointCount);
    for (size_t i = 0; i < pointCount; i++)
    {
        curve.points[i] = FCurvePoint{
            .position = {unpack[i * 4 + 0], unpack[i * 4 + 1], unpack[i * 4 + 2]},
            .radius = unpack[i * 4 + 3],
        };
    }

    curve.curveVertexCounts.resize(src->curve_vertex_counts->count);
    uint64_t referencedPoints = 0;
    for (size_t i = 0; i < src->curve_vertex_counts->count; i++)
    {
        cgltf_uint count = 0;
        CHECK(cgltf_accessor_read_uint(src->curve_vertex_counts, i, &count, 1));
        curve.curveVertexCounts[i] = count;
        CHECK_MSG(count >= 4 && (count - 1) % 3 == 0,
                  "Bezier curve strands must contain 3n + 1 controls, got {}", count);
        referencedPoints += count;
    }
    CHECK_MSG(referencedPoints == pointCount, "Curve strands reference {} points, but points accessor stores {}",
              referencedPoints, pointCount);
}

size_t GetSceneWorkerCount()
{
    return std::max<size_t>(1u, std::thread::hardware_concurrency());
}

size_t GetSceneTaskQueueSize(size_t taskCount)
{
    CHECK(taskCount > 0);
    return ThreadPool::CalcTaskSize(taskCount);
}

struct FBlobJob
{
    Vector<unsigned char> bytes;
    uint32_t count{0};
    uint32_t stride{0};
    uint64_t alignment{16};
    FBlobCodec requestedCodec{FBlobCodec::None};
    FBlobRef* outRef{nullptr};

    explicit FBlobJob(Allocator* alloc = GLOBAL_ALLOC)
        : bytes(alloc)
    {
    }
};

struct FPreparedBlob
{
    Span<const unsigned char> storedBytes;
    uint64_t decodedSize{0};
    uint32_t count{0};
    uint32_t stride{0};
    uint64_t alignment{16};
    FBlobCodec codec{FBlobCodec::None};
    FBlobRef* outRef{nullptr};
    Vector<unsigned char> ownedStorage;

    explicit FPreparedBlob(Allocator* alloc = GLOBAL_ALLOC)
        : ownedStorage(alloc)
    {
    }
};

struct FResourceBlobJobs
{
    Vector<FBlobJob> jobs;

    explicit FResourceBlobJobs(Allocator* alloc = GLOBAL_ALLOC)
        : jobs(alloc)
    {
    }
};

void AppendBytesBlobJob(Vector<FBlobJob>& jobs, Vector<unsigned char>&& bytes, uint32_t count, uint32_t stride,
                        FBlobCodec codec, FBlobRef& outRef, uint64_t alignment = 16)
{
    CHECK(stride != 0);
    uint64_t decodedSize = uint64_t(count) * stride;
    CHECK_MSG(decodedSize == bytes.size(), "Blob size mismatch: {} bytes for {} elements with stride {}",
              bytes.size(), count, stride);

    FBlobJob job(jobs.get_allocator().mResource);
    job.bytes = std::move(bytes);
    job.count = count;
    job.stride = stride;
    job.alignment = alignment;
    job.requestedCodec = codec;
    job.outRef = &outRef;
    jobs.push_back(std::move(job));
}

template <typename T>
void AppendArrayBlobJob(Vector<FBlobJob>& jobs, Vector<T> const& values, FBlobCodec codec, FBlobRef& outRef,
                        uint64_t alignment = 16)
{
    static_assert(std::is_trivially_copyable_v<T>);
    CHECK_MSG(values.size() <= UINT32_MAX, "FScene blob count exceeds uint32_t");
    CHECK_MSG(values.size() <= SIZE_MAX / sizeof(T), "FScene blob byte size exceeds size_t");

    FBlobJob job(jobs.get_allocator().mResource);
    size_t byteSize = values.size() * sizeof(T);
    job.bytes.resize(byteSize);
    if (byteSize != 0)
        std::memcpy(job.bytes.data(), values.data(), byteSize);
    job.count = static_cast<uint32_t>(values.size());
    job.stride = sizeof(T);
    job.alignment = alignment;
    job.requestedCodec = codec;
    job.outRef = &outRef;
    jobs.push_back(std::move(job));
}

void PrepareBlobJob(FBlobJob const& job, FPreparedBlob& prepared)
{
    CHECK(job.outRef != nullptr);
    CHECK(job.stride != 0);

    prepared.storedBytes = {job.bytes.data(), job.bytes.size()};
    prepared.decodedSize = uint64_t(job.count) * job.stride;
    prepared.count = job.count;
    prepared.stride = job.stride;
    prepared.alignment = job.alignment;
    prepared.codec = FBlobCodec::None;
    prepared.outRef = job.outRef;
    prepared.ownedStorage.clear();
    CHECK_MSG(prepared.decodedSize == job.bytes.size(),
              "Blob size mismatch: {} bytes for {} elements with stride {}", job.bytes.size(), job.count, job.stride);

    if (job.requestedCodec == FBlobCodec::None || job.bytes.empty())
        return;

    CHECK_MSG(job.requestedCodec == FBlobCodec::LZ4, "Unsupported blob codec {}",
              static_cast<uint32_t>(job.requestedCodec));
    CHECK_MSG(job.bytes.size() <= static_cast<size_t>(INT_MAX), "LZ4 blob too large: {} bytes", job.bytes.size());
    int maxCompressedSize = LZ4_compressBound(static_cast<int>(job.bytes.size()));
    CHECK_MSG(maxCompressedSize > 0, "LZ4 compression bound failed for {} bytes", job.bytes.size());
    prepared.ownedStorage.resize(static_cast<size_t>(maxCompressedSize));
    int compressedSize = LZ4_compress_default(reinterpret_cast<const char*>(job.bytes.data()),
                                              reinterpret_cast<char*>(prepared.ownedStorage.data()),
                                              static_cast<int>(job.bytes.size()), maxCompressedSize);
    CHECK_MSG(compressedSize > 0, "LZ4 compression failed for {} bytes", job.bytes.size());
    if (static_cast<size_t>(compressedSize) < job.bytes.size())
    {
        prepared.ownedStorage.resize(static_cast<size_t>(compressedSize));
        prepared.storedBytes = {prepared.ownedStorage.data(), prepared.ownedStorage.size()};
        prepared.codec = FBlobCodec::LZ4;
    }
    else
    {
        prepared.ownedStorage.clear();
    }
}

FBlobRef CommitPreparedBlob(FBlobSerializer& blobSerializer, FPreparedBlob const& blob)
{
    CHECK(blob.outRef != nullptr);
    CHECK(blob.stride != 0);

    FBlobRef ref{};
    ref.decodedSize = blob.decodedSize;
    ref.storedSize = blob.storedBytes.size();
    ref.count = blob.count;
    ref.stride = blob.stride;
    ref.codec = blob.codec;
    if (blob.storedBytes.empty())
        return ref;

    uint64_t payloadOffset = 0;
    Span<unsigned char> dst = blobSerializer.Allocate(blob.storedBytes.size(), blob.alignment, payloadOffset);
    ref.offset = payloadOffset;
    std::memcpy(dst.data(), blob.storedBytes.data(), blob.storedBytes.size());
    return ref;
}

void CommitPreparedBlobJobs(FBlobSerializer& blobSerializer, Vector<FPreparedBlob> const& preparedBlobs)
{
    for (FPreparedBlob const& blob : preparedBlobs)
        *blob.outRef = CommitPreparedBlob(blobSerializer, blob);
}

void AppendResourceBlobJobs(Vector<FBlobJob>& blobJobs, FResourceBlobJobs& resourceBlobJobs)
{
    blobJobs.reserve(blobJobs.size() + resourceBlobJobs.jobs.size());
    for (FBlobJob& job : resourceBlobJobs.jobs)
        blobJobs.push_back(std::move(job));
    resourceBlobJobs.jobs.clear();
}

void BuildTextureBlobJobs(FSerializedTexture& desc, Vector<FBlobJob>& blobJobs, FTexture&& texture);
void BuildMeshBlobJobs(FSerializedMesh& desc, Vector<FBlobJob>& blobJobs, FImportedMesh const& mesh, int32_t skeleton,
                       int32_t morphTrack);
void BuildCurveBlobJobs(FSerializedCurve& desc, Vector<FBlobJob>& blobJobs, FImportedCurve const& curve);

// Builds a flat, topologically sorted @ref FSkeleton from a glTF skin. @p outRemap maps a
// skin-local joint index (as stored in JOINTS_0) to its index in the sorted skeleton; rest TRS
// come from each joint node's local transform and parents above the skin are treated as identity
// (the skeleton evaluates in its own space). Inverse-bind matrices come from the skin accessor.
FSkeleton BuildSkeletonFromSkin(cgltf_data* data, const cgltf_skin* skin, Vector<uint16_t>& outRemap,
                                Allocator* alloc)
{
    FSkeleton skel(alloc);
    size_t n = skin->joints_count;
    outRemap.assign(n, 0);
    if (n == 0)
        return skel;

    // Parent index within the joint set (-1 if the parent node isn't a joint of this skin).
    Vector<int32_t> parentLocal(n, -1, alloc);
    for (size_t k = 0; k < n; k++)
    {
        cgltf_node* parent = skin->joints[k]->parent;
        if (parent)
            for (size_t m = 0; m < n; m++)
                if (skin->joints[m] == parent)
                {
                    parentLocal[k] = static_cast<int32_t>(m);
                    break;
                }
    }
    // Depth from a root; sorting by depth yields a parent-before-child order.
    Vector<uint32_t> depth(n, 0, alloc);
    for (size_t k = 0; k < n; k++)
    {
        uint32_t d = 0;
        int32_t c = static_cast<int32_t>(k);
        while (parentLocal[c] >= 0 && d <= n)
        {
            d++;
            c = parentLocal[c];
        }
        depth[k] = d;
    }
    Vector<uint32_t> order(n, 0, alloc);
    for (size_t k = 0; k < n; k++)
        order[k] = static_cast<uint32_t>(k);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return depth[a] < depth[b]; });
    for (uint32_t newIdx = 0; newIdx < n; newIdx++)
        outRemap[order[newIdx]] = static_cast<uint16_t>(newIdx);

    Vector<mat4> inverseBind(n, mat4(1.0f), alloc);
    if (skin->inverse_bind_matrices)
        cgltf_accessor_unpack_floats(skin->inverse_bind_matrices, reinterpret_cast<float*>(inverseBind.data()), n * 16);

    skel.joints.resize(n);
    for (uint32_t newIdx = 0; newIdx < n; newIdx++)
    {
        uint32_t oldIdx = order[newIdx];
        FJoint& joint = skel.joints[newIdx];
        joint.parent = parentLocal[oldIdx] >= 0 ? static_cast<int32_t>(outRemap[parentLocal[oldIdx]]) : -1;
        mat4 local;
        cgltf_node_transform_local(skin->joints[oldIdx], reinterpret_cast<float*>(&local));
        decompose(local, joint.restScale, joint.restRotation, joint.restTranslation);
        joint.inverseBind = inverseBind[oldIdx];
    }
    return skel;
}

FAnimInterp MapAnimInterp(cgltf_interpolation_type interp)
{
    switch (interp)
    {
    case cgltf_interpolation_type_step:
        return FAnimInterp::Step;
    case cgltf_interpolation_type_cubic_spline:
        return FAnimInterp::CubicSpline;
    default:
        return FAnimInterp::Linear;
    }
}

bool MapAnimPath(cgltf_animation_path_type path, FAnimPath& out)
{
    switch (path)
    {
    case cgltf_animation_path_type_translation:
        out = FAnimPath::Translation;
        return true;
    case cgltf_animation_path_type_rotation:
        out = FAnimPath::Rotation;
        return true;
    case cgltf_animation_path_type_scale:
        out = FAnimPath::Scale;
        return true;
    default: // morph-target weights and invalid paths are not supported yet
        return false;
    }
}

// Reads one animation channel's keyframes into @p out for target joint @p joint. Returns false for
// unsupported paths (e.g. morph weights) or missing accessors; grows @p duration to the last key.
bool BuildAnimChannel(const cgltf_animation_channel* ch, uint32_t joint, FAnimChannel& out, float& duration)
{
    FAnimPath path;
    if (!ch->target_node || !ch->sampler || !MapAnimPath(ch->target_path, path))
        return false;
    const cgltf_accessor* input = ch->sampler->input;
    const cgltf_accessor* output = ch->sampler->output;
    if (!input || !output)
        return false;
    out.joint = joint;
    out.path = path;
    out.interp = MapAnimInterp(ch->sampler->interpolation);
    out.times.resize(input->count);
    cgltf_accessor_unpack_floats(input, out.times.data(), input->count);
    size_t outFloats = output->count * cgltf_num_components(output->type);
    out.values.resize(outFloats);
    cgltf_accessor_unpack_floats(output, out.values.data(), outFloats);
    if (input->count > 0)
        duration = std::max(duration, out.times[input->count - 1]);
    return true;
}

// Builds a flat, topologically sorted @ref FSkeleton over *every* glTF node (rest-local TRS,
// identity inverse-bind). This is the scene-node hierarchy that drives rigid node animation:
// instances/lights reference its joints, and @ref ComputeGlobals turns animated node-local TRS into
// world matrices (parents propagate to children). @p outNodeToJoint maps a glTF node index to its
// joint index in the sorted skeleton.
FSkeleton BuildSceneNodeSkeleton(cgltf_data* data, Vector<int32_t>& outNodeToJoint, Allocator* alloc)
{
    FSkeleton skel(alloc);
    size_t n = data->nodes_count;
    outNodeToJoint.assign(n, -1);
    if (n == 0)
        return skel;

    Vector<int32_t> parentLocal(n, -1, alloc);
    for (size_t k = 0; k < n; k++)
        if (data->nodes[k].parent)
            parentLocal[k] = static_cast<int32_t>(cgltf_node_index(data, data->nodes[k].parent));
    Vector<uint32_t> depth(n, 0, alloc);
    for (size_t k = 0; k < n; k++)
    {
        uint32_t d = 0;
        int32_t c = static_cast<int32_t>(k);
        while (parentLocal[c] >= 0 && d <= n)
        {
            d++;
            c = parentLocal[c];
        }
        depth[k] = d;
    }
    Vector<uint32_t> order(n, 0, alloc);
    for (size_t k = 0; k < n; k++)
        order[k] = static_cast<uint32_t>(k);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return depth[a] < depth[b]; });
    for (uint32_t newIdx = 0; newIdx < n; newIdx++)
        outNodeToJoint[order[newIdx]] = static_cast<int32_t>(newIdx);

    skel.joints.resize(n);
    for (uint32_t newIdx = 0; newIdx < n; newIdx++)
    {
        uint32_t oldIdx = order[newIdx];
        FJoint& joint = skel.joints[newIdx];
        joint.parent = parentLocal[oldIdx] >= 0 ? outNodeToJoint[parentLocal[oldIdx]] : -1;
        mat4 local;
        cgltf_node_transform_local(&data->nodes[oldIdx], reinterpret_cast<float*>(&local));
        decompose(local, joint.restScale, joint.restRotation, joint.restTranslation);
        joint.inverseBind = mat4(1.0f); // rigid nodes don't skin
    }
    return skel;
}

void BuildGLTFSerializedScene(StringView path, FImportedScene& scene, Allocator* scratchAlloc)
{
    LOG(Scene, LogInfo, "Load GLTF Scene {}", path);
    CHECK(scene.mWriting);
    CHECK(scene.mFile != nullptr);
    CHECK(scratchAlloc != nullptr);
    FBlobSerializer blobSerializer(*scene.mFile, scene.mWriteOffset, scene.mHeader.payloadOffset, scratchAlloc);
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&options, path.data(), &data);
    CHECK_MSG(result == cgltf_result_success, "Scene load failure: {}", static_cast<int>(result));
    UniquePtr<cgltf_data, decltype(&cgltf_free)> raii(data, &cgltf_free);
    result = cgltf_load_buffers(&options, data, path.data());
    CHECK_MSG(result == cgltf_result_success, "Buffer load failure: {}", static_cast<int>(result));
    result = cgltf_validate(data);
    CHECK_MSG(result == cgltf_result_success, "Scene validate failure: {}", static_cast<int>(result));

    FSceneGlobals globals = scene.GetSceneGlobals();
    LoadFoundationColorManagementExtension(data, globals);
    scene.Set(globals);
    FLight environmentLight = MakeDefaultEnvironmentLight();
    Optional<String> environmentTexturePath = LoadFoundationEnvironmentExtension(data, path, environmentLight);

    /* Materials */
    // NOTE: Material 0 is reserved as the default material:
    // https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#default-material
    scene.Add(FMaterial{
        .baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f},
    });
    // Extra texture flags. Mostly used for sRGB to linear conversion
    constexpr unsigned kTextureInSRGB = 1 << 0;
    Vector<unsigned> textureFlags(data->textures_count, 0, scratchAlloc);
    auto assignTextureIndex = [&](cgltf_texture_view const& view, unsigned flags = 0u) -> uint32_t
    {
        if (!view.texture)
            return kInvalidTexture;
        size_t index = cgltf_texture_index(data, view.texture);
        CHECK_MSG(index < data->textures_count, "glTF texture index out of range");
        textureFlags[index] |= flags;
        return static_cast<uint32_t>(index);
    };
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
                material.baseColorTexture = assignTextureIndex(mat->pbr_metallic_roughness.base_color_texture, kTextureInSRGB);
            if (mat->pbr_metallic_roughness.metallic_roughness_texture.texture)
                material.metallicRoughnessTexture = assignTextureIndex(mat->pbr_metallic_roughness.metallic_roughness_texture);
        }
        if (mat->has_pbr_specular_glossiness)
        {
            const auto& sg = mat->pbr_specular_glossiness;
            material.baseColorFactor = {sg.diffuse_factor[0], sg.diffuse_factor[1], sg.diffuse_factor[2], sg.diffuse_factor[3]};
            material.roughnessFactor = 1.0f - sg.glossiness_factor;
            float specLuminance = max(sg.specular_factor[0], max(sg.specular_factor[1], sg.specular_factor[2]));
            constexpr float kDielectricF0 = 0.04f;
            material.metallicFactor = std::clamp((specLuminance - kDielectricF0) / (1.0f - kDielectricF0), 0.0f, 1.0f);
            if (sg.diffuse_texture.texture)
                material.baseColorTexture = assignTextureIndex(sg.diffuse_texture, kTextureInSRGB);
        }
        material.normalScale = mat->normal_texture.scale;
        if (mat->normal_texture.texture)
            material.normalTexture = assignTextureIndex(mat->normal_texture);
        if (mat->emissive_texture.texture)
            material.emissiveTexture = assignTextureIndex(mat->emissive_texture, kTextureInSRGB);
        material.emissiveFactor = {mat->emissive_factor[0], mat->emissive_factor[1], mat->emissive_factor[2], 1.0f};
        if (mat->emissive_strength.emissive_strength)
            material.emissiveFactor *= mat->emissive_strength.emissive_strength;
        material.transmissionFactor = mat->has_transmission ? mat->transmission.transmission_factor : 0.0f;
        if (mat->has_transmission && mat->transmission.transmission_texture.texture)
            material.transmissionTexture = assignTextureIndex(mat->transmission.transmission_texture);
        material.ior = mat->has_ior ? mat->ior.ior : 1.5f;
        material.specularFactor = mat->has_specular ? mat->specular.specular_factor : 1.0f;
        if (mat->has_specular)
        {
            material.specularColorFactor = {
                mat->specular.specular_color_factor[0],
                mat->specular.specular_color_factor[1],
                mat->specular.specular_color_factor[2]
            };
            if (mat->specular.specular_texture.texture)
                material.specularTexture = assignTextureIndex(mat->specular.specular_texture);
            if (mat->specular.specular_color_texture.texture)
                material.specularColorTexture = assignTextureIndex(mat->specular.specular_color_texture, kTextureInSRGB);
        }
        if (mat->has_anisotropy)
        {
            material.anisotropyStrength = std::clamp(mat->anisotropy.anisotropy_strength, 0.0f, 1.0f);
            material.anisotropyRotation = mat->anisotropy.anisotropy_rotation;
            if (mat->anisotropy.anisotropy_texture.texture)
                material.anisotropyTexture = assignTextureIndex(mat->anisotropy.anisotropy_texture);
        }
        if (mat->has_sheen)
        {
            material.sheenColorFactor = {
                mat->sheen.sheen_color_factor[0],
                mat->sheen.sheen_color_factor[1],
                mat->sheen.sheen_color_factor[2]
            };
            material.sheenRoughnessFactor = std::clamp(mat->sheen.sheen_roughness_factor, 0.0f, 1.0f);
            if (mat->sheen.sheen_color_texture.texture)
                material.sheenColorTexture = assignTextureIndex(mat->sheen.sheen_color_texture, kTextureInSRGB);
            if (mat->sheen.sheen_roughness_texture.texture)
                material.sheenRoughnessTexture = assignTextureIndex(mat->sheen.sheen_roughness_texture);
        }
        if (mat->has_clearcoat)
        {
            material.clearcoatFactor = std::clamp(mat->clearcoat.clearcoat_factor, 0.0f, 1.0f);
            material.clearcoatRoughnessFactor = std::clamp(mat->clearcoat.clearcoat_roughness_factor, 0.0f, 1.0f);
            if (mat->clearcoat.clearcoat_texture.texture)
                material.clearcoatTexture = assignTextureIndex(mat->clearcoat.clearcoat_texture);
            if (mat->clearcoat.clearcoat_roughness_texture.texture)
                material.clearcoatRoughnessTexture = assignTextureIndex(mat->clearcoat.clearcoat_roughness_texture);
        }
        material.subsurfaceFactor = 0.0f;
        material.subsurfaceColor = {1.0f, 1.0f, 1.0f};
        material.subsurfaceRadius = {1.0f, 0.2f, 0.1f};
        material.subsurfaceScale = 0.05f;
        if (mat->has_subsurface)
        {
            material.subsurfaceFactor = std::clamp(mat->subsurface.subsurface_weight, 0.0f, 1.0f);
            material.subsurfaceRadius = {
                mat->subsurface.subsurface_radius[0],
                mat->subsurface.subsurface_radius[1],
                mat->subsurface.subsurface_radius[2]
            };
            material.subsurfaceScale = mat->subsurface.subsurface_scale;
        }
        material.hairBetaM = material.roughnessFactor;
        material.hairBetaN = material.roughnessFactor;
        LoadFoundationMaterialExtension(mat, material);
        scene.Add(material);
    }

    Vector<FBlobJob> blobJobs(scratchAlloc);

    /* Textures */
    FTexture textureCodecInit(scratchAlloc);
    CHECK_MSG(data->textures_count <= UINT32_MAX, "glTF texture count exceeds uint32_t");
    CHECK_MSG(!environmentTexturePath.has_value() || data->textures_count < UINT32_MAX,
              "glTF texture count leaves no room for environment texture");
    size_t const sceneTextureCount = data->textures_count + (environmentTexturePath.has_value() ? 1u : 0u);
    uint32_t const environmentTextureIndex = environmentTexturePath.has_value()
        ? static_cast<uint32_t>(data->textures_count)
        : kInvalidTexture;
    scene.mTables.textures.clear();
    scene.mTables.textures.reserve(sceneTextureCount);
    for (size_t i = 0; i < sceneTextureCount; i++)
        scene.mTables.textures.emplace_back(scratchAlloc);
    Vector<FResourceBlobJobs> textureBlobJobs(scratchAlloc);
    textureBlobJobs.reserve(sceneTextureCount);
    for (size_t i = 0; i < sceneTextureCount; i++)
        textureBlobJobs.emplace_back(scratchAlloc);
    if (data->textures_count != 0)
    {
        ThreadPool pool(GetSceneWorkerCount(), GetSceneTaskQueueSize(data->textures_count), scratchAlloc, "SceneTexture");
        Vector<Future<void>> futures(scratchAlloc);
        futures.reserve(data->textures_count);
        for (size_t i = 0; i < data->textures_count; i++)
        {
            futures.push_back(pool.Push(
                [&, i]
                {
                    cgltf_texture* src = &data->textures[i];
                    String name = src->name ? src->name : fmt::format("{}_{}", path, i);
                    unsigned flags = textureFlags[i];
                    FTexture texture(scratchAlloc);
                    LOG(Scene, LogInfo, "Loading texture {}", name);
                    auto loaded = LoadGLTFTexture(src, path, scratchAlloc, flags & kTextureInSRGB);
                    if (loaded.has_value())
                    {
                        if (loaded->GetFormat() == RHIResourceFormat::R8G8B8A8Unorm ||
                            loaded->GetFormat() == RHIResourceFormat::R8G8B8A8Srgb)
                        {
                            loaded->GenerateMips();
                            texture = loaded->EncodeBC7(scratchAlloc);
                        }
                        else
                            texture = loaded.value();
                        LOG(Scene, LogInfo, "Loaded texture {}", name);
                    }
                    else
                    {
                        LOG(Scene, LogWarn, "No texture loaded for {}", name);
                    }
                    BuildTextureBlobJobs(scene.mTables.textures[i], textureBlobJobs[i].jobs, std::move(texture));
                }));
        }
        pool.Join();
        for (Future<void>& future : futures)
            future.get();
        for (size_t i = 0; i < data->textures_count; ++i)
            AppendResourceBlobJobs(blobJobs, textureBlobJobs[i]);
    }
    if (environmentTexturePath.has_value())
    {
        CHECK_MSG(environmentTextureIndex != kInvalidTexture, "Invalid environment texture index");
        FTexture environmentTexture(scratchAlloc);
        LOG(Scene, LogInfo, "Loading environment HDRI {}", *environmentTexturePath);
        auto result = LoadTexture(*environmentTexturePath, scratchAlloc, true);
        if (!result)
        {
            LOG(Scene, LogError, "Failed to load environment HDRI {}", *environmentTexturePath);
            return;
        }
        environmentTexture = std::move(result.value());
        BuildTextureBlobJobs(scene.mTables.textures[environmentTextureIndex],
                             textureBlobJobs[environmentTextureIndex].jobs, std::move(environmentTexture));
        AppendResourceBlobJobs(blobJobs, textureBlobJobs[environmentTextureIndex]);
        environmentLight.environmentTexture = environmentTextureIndex;
    }

    /* Skeletons (one per glTF skin) + a glTF-mesh -> skin map for skinned-mesh import */
    Vector<Vector<uint16_t>> skinRemap(scratchAlloc);
    skinRemap.reserve(data->skins_count);
    scene.mTables.skeletons.reserve(data->skins_count);
    for (size_t s = 0; s < data->skins_count; s++)
    {
        Vector<uint16_t> remap(scratchAlloc);
        scene.mTables.skeletons.push_back(BuildSkeletonFromSkin(data, &data->skins[s], remap, scratchAlloc));
        skinRemap.push_back(std::move(remap));
    }
    Vector<int32_t> meshToSkin(data->meshes_count, -1, scratchAlloc);
    for (size_t i = 0; i < data->nodes_count; i++)
    {
        const cgltf_node* node = &data->nodes[i];
        if (node->mesh && node->skin)
            meshToSkin[cgltf_mesh_index(data, node->mesh)] = static_cast<int32_t>(cgltf_skin_index(data, node->skin));
    }

    /* Scene-node hierarchy + rigid (non-skin) animation clips. Built before the instance loop so
     * instances/lights can record their scene-node index. Skin joints are excluded here (they are
     * driven by the skin clip path), so no node is animated twice. The hierarchy is only kept when
     * at least one rigid clip exists. */
    int32_t sceneNodeSkeleton = -1;
    Vector<int32_t> nodeToSceneJoint(scratchAlloc);
    if (data->animations_count > 0 && data->nodes_count > 0)
    {
        FSkeleton sceneNodes = BuildSceneNodeSkeleton(data, nodeToSceneJoint, scratchAlloc);
        int32_t const candidateSkeleton = static_cast<int32_t>(scene.mTables.skeletons.size());
        auto isSkinJoint = [&](cgltf_node* node)
        {
            for (size_t s = 0; s < data->skins_count; s++)
                for (size_t l = 0; l < data->skins[s].joints_count; l++)
                    if (data->skins[s].joints[l] == node)
                        return true;
            return false;
        };
        Vector<FAnimationClip> rigidClips(scratchAlloc);
        for (size_t a = 0; a < data->animations_count; a++)
        {
            const cgltf_animation* anim = &data->animations[a];
            FAnimationClip clip(scratchAlloc);
            clip.skeleton = candidateSkeleton;
            for (size_t c = 0; c < anim->channels_count; c++)
            {
                const cgltf_animation_channel* ch = &anim->channels[c];
                if (!ch->target_node || isSkinJoint(ch->target_node))
                    continue;
                int32_t const joint = nodeToSceneJoint[cgltf_node_index(data, ch->target_node)];
                if (joint < 0)
                    continue;
                FAnimChannel channel(scratchAlloc);
                if (!BuildAnimChannel(ch, static_cast<uint32_t>(joint), channel, clip.duration))
                    continue;
                clip.channels.push_back(std::move(channel));
            }
            if (!clip.channels.empty())
                rigidClips.push_back(std::move(clip));
        }
        if (!rigidClips.empty())
        {
            scene.mTables.skeletons.push_back(std::move(sceneNodes));
            sceneNodeSkeleton = candidateSkeleton;
            for (FAnimationClip& clip : rigidClips)
                scene.mTables.clips.push_back(std::move(clip));
        }
    }
    scene.mTables.sceneNodeSkeleton = sceneNodeSkeleton;
    auto NodeJoint = [&](size_t nodeIndex) -> int32_t
    { return sceneNodeSkeleton >= 0 ? nodeToSceneJoint[nodeIndex] : -1; };

    /* Morph-target weight tracks: one per glTF mesh that an animation drives via a `weights` channel
     * (first wins). Each mesh's submeshes link to it through FSerializedMesh::morphTrack. */
    Vector<int32_t> meshToMorphTrack(data->meshes_count, -1, scratchAlloc);
    for (size_t a = 0; a < data->animations_count; a++)
    {
        const cgltf_animation* anim = &data->animations[a];
        for (size_t c = 0; c < anim->channels_count; c++)
        {
            const cgltf_animation_channel* ch = &anim->channels[c];
            if (ch->target_path != cgltf_animation_path_type_weights || !ch->target_node ||
                !ch->target_node->mesh || !ch->sampler)
                continue;
            size_t meshIdx = cgltf_mesh_index(data, ch->target_node->mesh);
            if (meshToMorphTrack[meshIdx] >= 0)
                continue; // first track wins for a given mesh
            const cgltf_primitive* prim0 = ch->target_node->mesh->primitives_count ? &ch->target_node->mesh->primitives[0] : nullptr;
            uint32_t targetCount = prim0 ? static_cast<uint32_t>(prim0->targets_count) : 0u;
            const cgltf_accessor* input = ch->sampler->input;
            const cgltf_accessor* output = ch->sampler->output;
            if (targetCount == 0 || !input || !output)
                continue;
            FMorphTrack track(scratchAlloc);
            track.targetCount = targetCount;
            track.interp = MapAnimInterp(ch->sampler->interpolation);
            track.times.resize(input->count);
            cgltf_accessor_unpack_floats(input, track.times.data(), input->count);
            track.values.resize(output->count * cgltf_num_components(output->type));
            cgltf_accessor_unpack_floats(output, track.values.data(), track.values.size());
            if (input->count > 0)
                track.duration = track.times[input->count - 1];
            meshToMorphTrack[meshIdx] = static_cast<int32_t>(scene.mTables.morphTracks.size());
            scene.mTables.morphTracks.push_back(std::move(track));
        }
    }

    /* Meshes */
    size_t numSubmeshes = 0;
    for (size_t i = 0; i < data->meshes_count; i++)
        numSubmeshes += data->meshes[i].primitives_count;
    scene.mTables.meshes.reserve(numSubmeshes);
    for (size_t i = 0; i < numSubmeshes; i++)
        scene.mTables.meshes.emplace_back(scratchAlloc);
    Vector<FResourceBlobJobs> meshBlobJobs(scratchAlloc);
    meshBlobJobs.reserve(numSubmeshes);
    for (size_t i = 0; i < numSubmeshes; i++)
        meshBlobJobs.emplace_back(scratchAlloc);
    Vector<Pair<size_t, size_t>> submeshIndices(scratchAlloc);
    submeshIndices.reserve(data->meshes_count);
    uint32_t nextSubmesh = 0;
    if (numSubmeshes != 0)
    {
        ThreadPool pool(GetSceneWorkerCount(), GetSceneTaskQueueSize(numSubmeshes), scratchAlloc, "SceneMesh");
        Vector<Future<void>> futures(scratchAlloc);
        futures.reserve(numSubmeshes);
        for (size_t i = 0; i < data->meshes_count; i++)
        {
            auto& mesh = data->meshes[i];
            int32_t skinIndex = meshToSkin[i];
            int32_t morphTrack = meshToMorphTrack[i];
            const Vector<uint16_t>* remap = skinIndex >= 0 ? &skinRemap[skinIndex] : nullptr;
            auto& [mmin, mmax] = submeshIndices.emplace_back(nextSubmesh, nextSubmesh);
            for (size_t p = 0; p < mesh.primitives_count; p++)
            {
                auto* sub = mesh.primitives + p;
                CHECK(sub->type == cgltf_primitive_type_triangles);
                uint32_t meshIndex = nextSubmesh++;
                futures.push_back(pool.Push(
                    [&, meshIndex, sub, skinIndex, morphTrack, remap]
                    {
                        FImportedMesh submesh = LoadGLTFSubmesh(sub, scratchAlloc, remap, morphTrack >= 0);
                        // Deforming meshes (skinned or morph-animated) take the dynamic vertex/index
                        // path: skip vertex reordering (which would desync the per-vertex binding /
                        // morph deltas) and the DAG/meshlet build.
                        bool const dynamic = !submesh.skin.empty() || morphTrack >= 0;
                        if (!dynamic)
                        {
                            LOG(Meshopt, LogInfo, "Optimizing submesh {}, vtx: {}, idx: {}", meshIndex,
                                submesh.vertices.size(), submesh.lods[0].indices.size());
                            submesh.Optimize();
                            submesh.ClusterizeDAG();
                            LOG(Meshopt, LogInfo, "Optimized {}", meshIndex);
                        }
                        submesh.Quantize();
                        BuildMeshBlobJobs(scene.mTables.meshes[meshIndex], meshBlobJobs[meshIndex].jobs, submesh,
                                          submesh.skin.empty() ? -1 : skinIndex, morphTrack);
                    }));
            }
            mmax = nextSubmesh;
        }
        pool.Join();
        for (Future<void>& future : futures)
            future.get();
        for (FResourceBlobJobs& jobs : meshBlobJobs)
            AppendResourceBlobJobs(blobJobs, jobs);
    }
    else
    {
        for (size_t i = 0; i < data->meshes_count; i++)
            submeshIndices.emplace_back(nextSubmesh, nextSubmesh);
    }
    CHECK_MSG(nextSubmesh == numSubmeshes, "Serialized mesh count mismatch");

    /* Curves */
    scene.mTables.curves.resize(data->curves_count);
    Vector<FResourceBlobJobs> curveBlobJobs(scratchAlloc);
    curveBlobJobs.reserve(data->curves_count);
    for (size_t i = 0; i < data->curves_count; i++)
        curveBlobJobs.emplace_back(scratchAlloc);
    if (data->curves_count != 0)
    {
        ThreadPool pool(GetSceneWorkerCount(), GetSceneTaskQueueSize(data->curves_count), scratchAlloc, "SceneCurve");
        Vector<Future<void>> futures(scratchAlloc);
        futures.reserve(data->curves_count);
        for (size_t i = 0; i < data->curves_count; i++)
        {
            futures.push_back(pool.Push(
                [&, i]
                {
                    FImportedCurve curve(scratchAlloc);
                    LoadGLTFCurve(data, &data->curves[i], curve, scratchAlloc);
                    BuildCurveBlobJobs(scene.mTables.curves[i], curveBlobJobs[i].jobs, curve);
                }));
        }
        pool.Join();
        for (Future<void>& future : futures)
            future.get();
        for (FResourceBlobJobs& jobs : curveBlobJobs)
            AppendResourceBlobJobs(blobJobs, jobs);
    }

    /* Blob compression and commit */
    Vector<FPreparedBlob> preparedBlobs(scratchAlloc);
    preparedBlobs.reserve(blobJobs.size());
    for (size_t i = 0; i < blobJobs.size(); i++)
        preparedBlobs.emplace_back(scratchAlloc);
    if (!blobJobs.empty())
    {
        ThreadPool pool(GetSceneWorkerCount(), GetSceneTaskQueueSize(blobJobs.size()), scratchAlloc, "SceneBlob");
        Vector<Future<void>> futures(scratchAlloc);
        futures.reserve(blobJobs.size());
        for (size_t i = 0; i < blobJobs.size(); i++)
        {
            futures.push_back(pool.Push(
                [&, i]
                {
                    PrepareBlobJob(blobJobs[i], preparedBlobs[i]);
                }));
        }
        pool.Join();
        for (Future<void>& future : futures)
            future.get();
        CommitPreparedBlobJobs(blobSerializer, preparedBlobs);
    }

    /* Instances / Cameras / Light */
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
            // Per EXT_mesh_gpu_instancing, the extension carries N parallel attribute accessors
            // (TRANSLATION/ROTATION/SCALE, optionally custom _* like _ID) that expand a single
            // node-mesh into N atomic instances. Each instance's local TRS is post-multiplied
            // onto the node's world matrix. We expand to flat FInstance entries (one per
            // submesh per instance) so downstream GPUScene treats them like any other instance.
            mat4 worldMat;
            cgltf_node_transform_world(node, reinterpret_cast<float*>(&worldMat));

            auto meshIndex = cgltf_mesh_index(data, node->mesh);
            auto [mmin, mmax] = submeshIndices[meshIndex];

            size_t instCount = 1;
            const cgltf_accessor* tAcc = nullptr;
            const cgltf_accessor* rAcc = nullptr;
            const cgltf_accessor* sAcc = nullptr;
            if (node->has_mesh_gpu_instancing && node->mesh_gpu_instancing.attributes_count > 0)
            {
                const cgltf_mesh_gpu_instancing& gi = node->mesh_gpu_instancing;
                instCount = gi.attributes[0].data->count;
                for (size_t k = 0; k < gi.attributes_count; ++k)
                {
                    const cgltf_attribute& attr = gi.attributes[k];
                    if (attr.name == nullptr) continue;
                    if (std::strcmp(attr.name, "TRANSLATION") == 0) tAcc = attr.data;
                    else if (std::strcmp(attr.name, "ROTATION") == 0) rAcc = attr.data;
                    else if (std::strcmp(attr.name, "SCALE") == 0) sAcc = attr.data;
                    // Other custom attributes (e.g. "_ID") are ignored for now.
                }
            }

            FInstance instance{};
            instance.type = FInstanceType::Mesh;
            instance.node = NodeJoint(i);

            for (size_t inst = 0; inst < instCount; ++inst)
            {
                FTransform xform{};
                if (instCount == 1 && tAcc == nullptr && rAcc == nullptr && sAcc == nullptr)
                {
                    // Fast path: no instancing, reuse node world directly.
                    decompose(worldMat, xform.scale, xform.rotation, xform.transform);
                }
                else
                {
                    float3 t{0.0f, 0.0f, 0.0f};
                    quat   r{1.0f, 0.0f, 0.0f, 0.0f}; // (w, x, y, z) identity
                    float3 s{1.0f, 1.0f, 1.0f};
                    if (tAcc)
                    {
                        float v[3] = {0, 0, 0};
                        cgltf_accessor_read_float(tAcc, inst, v, 3);
                        t = float3{v[0], v[1], v[2]};
                    }
                    if (rAcc)
                    {
                        float v[4] = {0, 0, 0, 1};
                        cgltf_accessor_read_float(rAcc, inst, v, 4);
                        // glTF stores quaternions as (x, y, z, w); glm::quat ctor is (w, x, y, z).
                        r = quat{v[3], v[0], v[1], v[2]};
                    }
                    if (sAcc)
                    {
                        float v[3] = {1, 1, 1};
                        cgltf_accessor_read_float(sAcc, inst, v, 3);
                        s = float3{v[0], v[1], v[2]};
                    }
                    mat4 instLocal = translate(mat4(1.0f), t) * mat4_cast(r) * scale(mat4(1.0f), s);
                    mat4 instWorld = worldMat * instLocal;
                    decompose(instWorld, xform.scale, xform.rotation, xform.transform);
                }
                instance.transform = xform;
                for (size_t j = mmin; j < mmax; j++)
                {
                    auto* sub = node->mesh->primitives + j - mmin;
                    instance.resourceIndex = static_cast<uint32_t>(j);
                    instance.materialIndex = sub->material ? cgltf_material_index(data, sub->material) + 1u : 0u;
                    scene.Add(instance);
                }
            }
        }
        if (node->curve)
        {
            FInstance instance{};
            getTransform(instance.transform);
            instance.type = FInstanceType::Curve;
            instance.node = NodeJoint(i);
            instance.resourceIndex = static_cast<uint32_t>(cgltf_curve_index(data, node->curve));
            instance.materialIndex = node->curve->material ? static_cast<uint32_t>(cgltf_material_index(data, node->curve->material) + 1u) : 0u;
            scene.Add(instance);
        }
        if (node->camera)
        {
            FCamera camera{};
            getTransform(camera.transform);
            camera.node = NodeJoint(i);
            camera.fovY = node->camera->data.perspective.yfov;
            if (node->camera->has_lens)
            {
                camera.lensEnabled = true;
                camera.sensorHeightMm = node->camera->lens.sensor_size;
                camera.fStop = node->camera->lens.fstop;
                camera.focusDistance = node->camera->lens.focus_distance;
                camera.apertureBlades = node->camera->lens.aperture_blades;
                camera.apertureRotation = node->camera->lens.aperture_rotation;
                camera.apertureRatio = node->camera->lens.aperture_ratio;
            }
            scene.Add(camera);
        }
        if (node->light)
        {
            FLight light{};
            getTransform(light.transform);
            light.node = NodeJoint(i);
            light.color = float3{node->light->color[0], node->light->color[1], node->light->color[2]};
            light.power = node->light->intensity / 683.0f;
            light.range = node->light->range;
            switch (node->light->type)
            {
            case cgltf_light_type_directional:
                light.type = FLightType::Directional;
                light.angularDiameter = node->light->has_foundation_lights ? node->light->angular_diameter : 0.0f;
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
            scene.Add(light);
        }
        if (node->light_area)
        {
            FLight light{};
            getTransform(light.transform);
            light.node = NodeJoint(i);
            float ws = std::max({std::abs(light.transform.scale.x), std::abs(light.transform.scale.y),
                                 std::abs(light.transform.scale.z)});
            light.transform.scale = float3{1, 1, 1};

            const cgltf_light_area* la = node->light_area;
            light.color = float3{la->color[0], la->color[1], la->color[2]};
            light.range = 0.0f;
            light.twoSided = false;
            light.normalize = true;

            float fullW = 1.0f, fullH = 1.0f;
            if (la->type == cgltf_light_area_type_disk)
            {
                light.type = FLightType::Disk;
                fullW = fullH = la->size * ws;
            }
            else
            {
                light.type = FLightType::Rect;
                fullW = la->size * la->rect_aspect * ws;
                fullH = la->size * ws;
            }
            light.width = fullW * 0.5f;
            light.height = fullH * 0.5f;
            light.power = la->intensity;
            scene.Add(light);
        }
    }

    /* Animations: one clip per glTF animation, bound to a single skeleton (the first one a channel
     * targets). Channels are resolved to that skeleton's topological joint indices; channels for
     * other skeletons or unsupported paths (morph weights) are dropped. */
    auto findJoint = [&](cgltf_node* node, int32_t& outSkin, uint32_t& outJoint) -> bool
    {
        for (size_t s = 0; s < data->skins_count; s++)
        {
            const cgltf_skin* skin = &data->skins[s];
            for (size_t l = 0; l < skin->joints_count; l++)
                if (skin->joints[l] == node)
                {
                    outSkin = static_cast<int32_t>(s);
                    outJoint = skinRemap[s][l];
                    return true;
                }
        }
        return false;
    };
    for (size_t a = 0; a < data->animations_count; a++)
    {
        const cgltf_animation* anim = &data->animations[a];
        int32_t clipSkin = -1;
        for (size_t c = 0; c < anim->channels_count && clipSkin < 0; c++)
        {
            int32_t s;
            uint32_t j;
            if (anim->channels[c].target_node && findJoint(anim->channels[c].target_node, s, j))
                clipSkin = s;
        }
        if (clipSkin < 0)
            continue; // no skinned target (rigid articulation is a later step)

        FAnimationClip clip(scratchAlloc);
        clip.skeleton = clipSkin;
        for (size_t c = 0; c < anim->channels_count; c++)
        {
            const cgltf_animation_channel* ch = &anim->channels[c];
            int32_t s;
            uint32_t j;
            if (!ch->target_node || !findJoint(ch->target_node, s, j) || s != clipSkin)
                continue;
            FAnimChannel channel(scratchAlloc);
            if (!BuildAnimChannel(ch, j, channel, clip.duration))
                continue;
            clip.channels.push_back(std::move(channel));
        }
        if (!clip.channels.empty())
            scene.mTables.clips.push_back(std::move(clip));
    }
    scene.mTables.lights.insert(scene.mTables.lights.begin(), environmentLight);
}

static_assert(std::is_trivially_copyable_v<FSceneHeader>);
static_assert(std::is_trivially_copyable_v<FSerializedMeshLOD>);
static_assert(std::is_trivially_copyable_v<FSerializedCurve>);

static constexpr uint32_t kGPUSceneRingFrameSlack = 3u;
static constexpr uint32_t kGPUScenePersistentTexture2DBindings = 2u; // GGX LUT + Sheen LTC LUT.
static constexpr uint32_t kGPUScenePersistentTexture3DBindings = 2u; // default SDR/HDR view LUTs.
static constexpr uint32_t kGPUSceneDefaultTextureBindings = 2u; // _FoundationDefault Texture2D + Texture2DFloat.
static constexpr uint32_t kGPUSceneEnvMapBindings = 3u; // Env map + marginal/conditional CDF textures.
static constexpr uint32_t kGPUSceneTextureBindingSlack = 8u;
static constexpr size_t kGPUSceneByteBudgetSlack = 64u << 10u;

uint32_t CountGPUSceneBudget(size_t count)
{
    count = std::max<size_t>(count, 1u);
    CHECK_MSG(count <= UINT32_MAX, "GPUScene count budget {} exceeds uint32_t range", count);
    return static_cast<uint32_t>(count);
}

uint32_t RingGPUSceneBudget(size_t count)
{
    return CountGPUSceneBudget(count * kGPUSceneRingFrameSlack);
}

uint32_t ByteGPUSceneBudget(size_t bytes, size_t minBytes, size_t alignment, size_t maxBytes = UINT32_MAX)
{
    if (bytes != 0)
        bytes += kGPUSceneByteBudgetSlack;
    size_t budget = AlignUp(std::max(bytes, minBytes), alignment);
    CHECK_MSG(budget <= maxBytes, "GPUScene byte budget {} exceeds maximum {}", budget, maxBytes);
    CHECK_MSG(budget <= UINT32_MAX, "GPUScene byte budget {} exceeds uint32_t range", budget);
    return static_cast<uint32_t>(budget);
}

void BuildTextureBlobJobs(FSerializedTexture& desc, Vector<FBlobJob>& blobJobs, FTexture&& texture)
{
    Allocator* alloc = desc.subresources.get_allocator().mResource;
    desc.magic = DDS_MAGIC;
    desc.header = {};
    desc.header10 = {};
    desc.subresources.clear();
    if (!texture.IsValid())
        return;

    CHECK_MSG(texture.bytes.size() <= UINT32_MAX, "FScene texture blob too large");
    desc.magic = texture.magic;
    desc.header = texture.header;
    desc.header10 = texture.header10;
    desc.subresources.resize(desc.GetSubresourceCount());

    for (uint32_t layer = 0; layer < desc.GetNumLayers(); ++layer)
    {
        for (uint32_t mip = 0; mip < desc.GetNumMips(); ++mip)
        {
            Span<const unsigned char> subresource = texture.GetSubresource(mip, layer);
            CHECK_MSG(subresource.size_bytes() <= UINT32_MAX, "FScene texture subresource blob too large");
            Vector<unsigned char> bytes(alloc);
            bytes.assign(subresource.begin(), subresource.end());
            FBlobRef& blob = desc.subresources[desc.GetSubresourceIndex(layer, mip)];
            AppendBytesBlobJob(blobJobs, std::move(bytes), static_cast<uint32_t>(subresource.size_bytes()),
                               sizeof(unsigned char), FBlobCodec::LZ4, blob);
        }
    }
}

uint32_t CalculateRenderableCurveSegmentCount(FImportedCurve const& curve)
{
    uint32_t segmentCount = 0;
    for (uint32_t count : curve.curveVertexCounts)
    {
        switch (curve.basis)
        {
        case FCurveBasis::Bezier:
            CHECK_MSG(count >= 4 && (count - 1) % 3 == 0,
                      "Bezier curve strands must contain 3n + 1 controls, got {}", count);
            segmentCount += (count - 1) / 3;
            break;
        case FCurveBasis::Linear:
            segmentCount += count > 1 ? count - 1 : 0;
            break;
        default:
            CHECK_MSG(false, "Unsupported curve basis {}", static_cast<uint32_t>(curve.basis));
            break;
        }
    }
    return segmentCount;
}

FSerializedCurveAABB BuildCurveAABB(float3 const& mn, float3 const& mx)
{
    return FSerializedCurveAABB{mn.x, mn.y, mn.z, mx.x, mx.y, mx.z};
}

void BuildCurveGeometry(FImportedCurve const& curve, Span<FSerializedCurveSegment> segments,
                        Span<FSerializedCurveAABB> aabbs)
{
    CHECK_MSG(segments.size() == aabbs.size(), "Curve geometry output size mismatch");

    uint32_t segmentCursor = 0;
    auto WriteLineSegment = [&](uint32_t p0, uint32_t p1, float u0, float u1)
    {
        CHECK(segmentCursor < segments.size());
        segments[segmentCursor] = FSerializedCurveSegment{.p0 = p0, .p1 = p1, .u0 = u0, .u1 = u1};

        const auto& a = curve.points[p0];
        const auto& b = curve.points[p1];
        float radius = std::max(a.radius, b.radius);
        float3 mn = min(a.position, b.position) - float3(radius);
        float3 mx = max(a.position, b.position) + float3(radius);
        aabbs[segmentCursor] = BuildCurveAABB(mn, mx);
        segmentCursor++;
    };
    auto WriteBezierSpan = [&](uint32_t p0, uint32_t p1, float u0, float u1)
    {
        CHECK(segmentCursor < segments.size());
        segments[segmentCursor] = FSerializedCurveSegment{.p0 = p0, .p1 = p1, .u0 = u0, .u1 = u1};

        const auto& a = curve.points[p0];
        const auto& b = curve.points[p0 + 1];
        const auto& c = curve.points[p0 + 2];
        const auto& d = curve.points[p1];
        float radius = std::max(std::max(a.radius, b.radius), std::max(c.radius, d.radius));
        float3 mn = min(min(a.position, b.position), min(c.position, d.position)) - float3(radius);
        float3 mx = max(max(a.position, b.position), max(c.position, d.position)) + float3(radius);
        aabbs[segmentCursor] = BuildCurveAABB(mn, mx);
        segmentCursor++;
    };

    uint32_t pointCursor = 0;
    for (uint32_t count : curve.curveVertexCounts)
    {
        CHECK_MSG(pointCursor + count <= curve.points.size(), "Curve set references more points than it stores");
        if (count <= 1)
        {
            pointCursor += count;
            continue;
        }

        switch (curve.basis)
        {
        case FCurveBasis::Bezier:
        {
            CHECK_MSG(count >= 4 && (count - 1) % 3 == 0,
                      "Bezier curve strands must contain 3n + 1 controls, got {}", count);
            uint32_t spanCount = (count - 1) / 3;
            float invSpanCount = 1.0f / float(spanCount);
            for (uint32_t i = 0; i < spanCount; ++i)
            {
                uint32_t p0 = pointCursor + i * 3;
                uint32_t p1 = pointCursor + (i + 1) * 3;
                WriteBezierSpan(p0, p1, float(i) * invSpanCount, float(i + 1) * invSpanCount);
            }
            break;
        }
        case FCurveBasis::Linear:
        {
            float invSegmentCount = 1.0f / float(count - 1);
            for (uint32_t i = 0; i + 1 < count; ++i)
            {
                uint32_t p0 = pointCursor + i;
                uint32_t p1 = pointCursor + i + 1;
                WriteLineSegment(p0, p1, float(i) * invSegmentCount, float(i + 1) * invSegmentCount);
            }
            break;
        }
        default:
            CHECK_MSG(false, "Unsupported curve basis {}", static_cast<uint32_t>(curve.basis));
            break;
        }
        pointCursor += count;
    }

    CHECK_MSG(pointCursor == curve.points.size(), "Curve strands reference {} points, but points array stores {}",
              pointCursor, curve.points.size());
    CHECK_MSG(segmentCursor == segments.size(), "Curve segment count mismatch: expected {} got {}",
              segments.size(), segmentCursor);
}

void BuildMeshBlobJobs(FSerializedMesh& desc, Vector<FBlobJob>& blobJobs, FImportedMesh const& mesh, int32_t skeleton,
                       int32_t morphTrack)
{
    CHECK_MSG(!mesh.verticesQuantized.empty(), "FScene mesh is not quantized");
    desc.vertices = {};
    desc.vertexCount = static_cast<uint32_t>(mesh.verticesQuantized.size());
    desc.lods.clear();
    desc.dagGroups = {};
    desc.dagMeshlets = {};
    desc.dagMeshletTri = {};
    desc.dagMeshletVtx = {};
    desc.skinBinding = {};
    desc.skeleton = skeleton;
    desc.morphPositions = {};
    desc.morphTargetCount = mesh.morphTargetCount;
    desc.morphTrack = morphTrack;

    AppendArrayBlobJob(blobJobs, mesh.verticesQuantized, FBlobCodec::LZ4, desc.vertices);
    if (!mesh.skin.empty())
    {
        CHECK_MSG(mesh.skin.size() == mesh.verticesQuantized.size(), "Skin binding count mismatch");
        AppendArrayBlobJob(blobJobs, mesh.skin, FBlobCodec::LZ4, desc.skinBinding);
    }
    if (!mesh.morphPositions.empty())
    {
        CHECK_MSG(mesh.morphPositions.size() ==
                      static_cast<size_t>(mesh.morphTargetCount) * mesh.verticesQuantized.size(),
                  "Morph delta count mismatch");
        AppendArrayBlobJob(blobJobs, mesh.morphPositions, FBlobCodec::LZ4, desc.morphPositions);
    }

    desc.lods.reserve(mesh.lods.size());
    for (auto const& lod : mesh.lods)
    {
        FSerializedMeshLOD& lodDesc = desc.lods.emplace_back();
        lodDesc.indexCount = static_cast<uint32_t>(lod.indices.size());
        AppendArrayBlobJob(blobJobs, lod.indices, FBlobCodec::LZ4, lodDesc.indices);
    }

    AppendArrayBlobJob(blobJobs, mesh.dag.groups, FBlobCodec::LZ4, desc.dagGroups);
    AppendArrayBlobJob(blobJobs, mesh.dag.meshlets, FBlobCodec::LZ4, desc.dagMeshlets);
    AppendArrayBlobJob(blobJobs, mesh.dag.meshletTri, FBlobCodec::LZ4, desc.dagMeshletTri);
    AppendArrayBlobJob(blobJobs, mesh.dag.meshletVtx, FBlobCodec::LZ4, desc.dagMeshletVtx);
}

void BuildCurveBlobJobs(FSerializedCurve& desc, Vector<FBlobJob>& blobJobs, FImportedCurve const& curve)
{
    desc = {};
    desc.materialIndex = curve.materialIndex;
    uint32_t segmentCount = CalculateRenderableCurveSegmentCount(curve);

    Allocator* scratchAlloc = blobJobs.get_allocator().mResource;
    Vector<FSerializedCurveSegment> segments(scratchAlloc);
    Vector<FSerializedCurveAABB> aabbs(scratchAlloc);
    segments.resize(segmentCount);
    aabbs.resize(segmentCount);
    BuildCurveGeometry(curve, Span<FSerializedCurveSegment>(segments.data(), segments.size()),
                       Span<FSerializedCurveAABB>(aabbs.data(), aabbs.size()));

    AppendArrayBlobJob(blobJobs, curve.points, FBlobCodec::LZ4, desc.points);
    AppendArrayBlobJob(blobJobs, segments, FBlobCodec::LZ4, desc.segments);
    AppendArrayBlobJob(blobJobs, aabbs, FBlobCodec::LZ4, desc.aabbs, alignof(FSerializedCurveAABB));
}

FImportedScene::FImportedScene(MemoryMappedFile& file, Allocator* scratchAlloc)
    : mTables(scratchAlloc), mFile(&file), mScratchAlloc(scratchAlloc), mWriting(file.IsWritable())
{
    CHECK(scratchAlloc != nullptr);
    if (mWriting)
    {
        mHeader.headerSize = sizeof(FSceneHeader);
        mHeader.payloadOffset = AlignUpU64(sizeof(FSceneHeader), mHeader.payloadAlignment);
        mWriteOffset = mHeader.payloadOffset;
        EnsureMappedFileSize(file, mWriteOffset);
        std::memcpy(file.MutableData(), &mHeader, sizeof(mHeader));
        if (mHeader.payloadOffset > sizeof(mHeader))
            std::memset(file.MutableData() + sizeof(mHeader), 0, static_cast<size_t>(mHeader.payloadOffset - sizeof(mHeader)));
    }
}

Span<const unsigned char> FImportedScene::GetPayloadBytes() const
{
    CHECK(mFile != nullptr);
    CHECK(!mWriting);
    CHECK_MSG(mHeader.payloadOffset <= mHeader.metadataOffset, "FScene payload range is invalid");
    return {mFile->Data() + mHeader.payloadOffset, static_cast<size_t>(mHeader.metadataOffset - mHeader.payloadOffset)};
}

FBlobDeserializer FImportedScene::GetBlobDeserializer() const
{
    return FBlobDeserializer(GetPayloadBytes());
}

bool FImportedScene::ReadBlob(FBlobRef const& blob, void* dst, size_t size, Allocator* scratchAlloc) const
{
    return GetBlobDeserializer().ReadBytes(blob, dst, size, scratchAlloc);
}

GPUSceneDesc FImportedScene::CalculateGPUSceneDesc(Foundation::RHI::RHIDeviceCapabilities const& caps) const
{
    GPUSceneDesc desc{};
    size_t primitiveBytes = 0;
    for (auto const& mesh : GetMeshes())
    {
        primitiveBytes = AlignUp(primitiveBytes, size_t(4));
        primitiveBytes += GPUScene::CalculateMeshPrimitiveSize(mesh);
    }
    size_t curveAABBBytes = 0;
    for (auto const& curve : GetCurves())
    {
        primitiveBytes = AlignUp(primitiveBytes, size_t(4));
        primitiveBytes += GPUScene::CalculateCurvePrimitiveSize(curve);

        curveAABBBytes = AlignUp(curveAABBBytes, alignof(Foundation::RHI::RHIAccelerationStructureAABB));
        curveAABBBytes += GPUScene::CalculateCurveAABBSize(curve);
    }

    desc.primitiveBudget = ByteGPUSceneBudget(primitiveBytes, desc.primitiveBudget, size_t(4));
    desc.curveAABBBudget = ByteGPUSceneBudget(curveAABBBytes, desc.curveAABBBudget, alignof(Foundation::RHI::RHIAccelerationStructureAABB));

    size_t areaLightCount = 0;
    for (auto const& light : GetLights())
    {
        if (light.type == FLightType::Disk || light.type == FLightType::Rect)
            areaLightCount++;
    }
    const size_t tlasInstanceCount = GetInstances().size() + areaLightCount;
    desc.instanceBudget = RingGPUSceneBudget(GetInstances().size());
    desc.tlasInstanceBudget = RingGPUSceneBudget(tlasInstanceCount);
    desc.materialBudget = RingGPUSceneBudget(GetMaterials().size());
    desc.lightBudget = RingGPUSceneBudget(GetLights().size());
    desc.geometryBudget = CountGPUSceneBudget(GetMeshes().size() + GetCurves().size());

    // CPU-skinned meshes take the dynamic (CPU-updateable) geometry path. Size the per-slot ring
    // to the sum of their footprints (GSMesh header + quantized verts + LOD0 indices); 0 leaves the
    // feature off when the scene has no skinned meshes.
    size_t dynamicBytesPerSlot = 0;
    for (auto const& mesh : GetMeshes())
    {
        if (mesh.skinBinding.count == 0 && mesh.morphTrack < 0)
            continue;
        size_t vtxBytes = static_cast<size_t>(mesh.vertices.decodedSize);
        size_t idxBytes = mesh.lods.empty() ? 0u : static_cast<size_t>(mesh.lods[0].indices.decodedSize);
        dynamicBytesPerSlot = AlignUp(dynamicBytesPerSlot, size_t(16));
        dynamicBytesPerSlot += AlignUp(sizeof(GSMesh) + vtxBytes + idxBytes, size_t(16));
    }
    if (dynamicBytesPerSlot != 0)
    {
        desc.framesInFlight = kGPUSceneRingFrameSlack; // ring sized to framesInFlight + 1 internally
        desc.dynamicGeometryBudget = ByteGPUSceneBudget(dynamicBytesPerSlot, 0, size_t(16));
    }

    size_t textureBindings = kGPUScenePersistentTexture2DBindings + kGPUSceneDefaultTextureBindings +
        kGPUSceneTextureBindingSlack;
    FLight const* environmentLight = GetEnvironmentLight();
    for (size_t textureIndex = 0; textureIndex < GetTextures().size(); ++textureIndex)
    {
        if (environmentLight && environmentLight->environmentMap &&
            environmentLight->environmentTexture != kInvalidTexture && textureIndex == environmentLight->environmentTexture)
            continue;
        FSerializedTexture const& texture = GetTextures()[textureIndex];
        textureBindings += texture.IsValid() ? 1u : 0u;
    }
    if (environmentLight && environmentLight->environmentMap)
        textureBindings += kGPUSceneEnvMapBindings;
    textureBindings += kGPUScenePersistentTexture3DBindings;
    desc.texturesBudget = CountGPUSceneBudget(textureBindings);
    return desc;
}

void FinalizeSceneWriter(FImportedScene& scene)
{
    CHECK(scene.mWriting);
    CHECK(scene.mFile != nullptr);
    uint64_t payloadFileEnd = scene.mWriteOffset;
    CHECK_MSG(payloadFileEnd >= scene.mHeader.payloadOffset, "FScene writer is before payload offset");

    Vector<unsigned char> metadata(scene.mScratchAlloc);
    MemoryWriter metadataWriter(metadata);
    FSerialize(metadataWriter, scene.mTables);

    scene.mHeader.metadataOffset = AlignUpU64(payloadFileEnd, 16);
    scene.mHeader.metadataSize = metadata.size();
    scene.mHeader.fileSize = scene.mHeader.metadataOffset + scene.mHeader.metadataSize;

    EnsureMappedFileSize(*scene.mFile, scene.mHeader.fileSize);
    if (scene.mHeader.metadataOffset > payloadFileEnd)
        std::memset(scene.mFile->MutableData() + payloadFileEnd, 0,
                    static_cast<size_t>(scene.mHeader.metadataOffset - payloadFileEnd));
    std::memcpy(scene.mFile->MutableData() + scene.mHeader.metadataOffset, metadata.data(), metadata.size());
    std::memcpy(scene.mFile->MutableData(), &scene.mHeader, sizeof(scene.mHeader));
    scene.mFile->Flush(0, scene.mHeader.fileSize);
    scene.mFile->Resize(scene.mHeader.fileSize);
    scene.mWriteOffset = scene.mHeader.fileSize;
    scene.mWriting = false;
}

FImportedScene::~FImportedScene()
{
    if (mWriting)
        FinalizeSceneWriter(*this);
}

void LoadGLTF(StringView path, FImportedScene& scene, Allocator* scratchAlloc)
{
    CHECK(scene.mWriting);
    CHECK(scratchAlloc != nullptr);
    BuildGLTFSerializedScene(path, scene, scratchAlloc);
}

void LoadFSCN(FImportedScene& scene)
{
    CHECK(scene.mFile != nullptr);
    CHECK(!scene.mWriting);
    Span<const unsigned char> bytes = scene.mFile->Bytes();
    CHECK_MSG(bytes.size() >= sizeof(FSceneHeader), "FSCN file is smaller than its header");
    std::memcpy(&scene.mHeader, bytes.data(), sizeof(scene.mHeader));
    ValidateSceneHeader(scene.mHeader);
    CHECK_MSG(scene.mHeader.fileSize <= bytes.size(), "FScene header file size exceeds mapped file size");

    Span<const unsigned char> metadata(bytes.data() + scene.mHeader.metadataOffset,
                                       static_cast<size_t>(scene.mHeader.metadataSize));
    MemoryReader metadataReader(metadata);
    FDeserialize(metadataReader, scene.mTables);
    CHECK_MSG(metadataReader.tell() == metadata.size(), "FScene metadata has trailing or unread bytes");
    ValidateSceneTables(scene.mHeader, scene.mTables);
}

String LoadScene(StringView path, FImportedScene& scene, Allocator* scratchAlloc)
{
    CHECK(scratchAlloc != nullptr);
    auto ext = LowerExtension(std::filesystem::path(path.data()));
    if (ext == ".fscn")
    {
        CHECK(scene.mFile != nullptr);
        CHECK(!scene.mWriting);
        LoadFSCN(scene);
        return String(path);
    }

    CHECK(scene.mWriting);
    LoadGLTF(path, scene, scratchAlloc);
    return String(path);
}