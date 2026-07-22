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
#include <type_traits>
#include <RHICore/Device.hpp>
#include <Renderer/Postprocess.hpp>
#include <Renderer/GPUScene.hpp>
#include <Renderer/Mesh.hpp>
#include "Curve.hpp"

namespace
{
String DecodeURI(StringView encoded)
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

FSerializedBounds BuildMeshBounds(FImportedMesh const& mesh)
{
    if (mesh.vertices.empty())
        return {};

    FSerializedBounds bounds = FSerializedBounds::Empty();
    for (FVertex const& vertex : mesh.vertices)
        bounds += vertex.position;
    return bounds;
}

FSerializedBounds BuildCurveBounds(FImportedCurve const& curve)
{
    if (curve.segments.empty())
        return {};

    FSerializedBounds bounds = FSerializedBounds::Empty();
    for (FImportedCurveSegment const& segment : curve.segments)
    {
        bounds += segment.p0 - float3(segment.r0);
        bounds += segment.p0 + float3(segment.r0);
        bounds += segment.p1 - float3(segment.r1);
        bounds += segment.p1 + float3(segment.r1);
    }
    return bounds;
}

const cgltf_accessor* FindCustomAttribute(const cgltf_primitive* prim, char const* name)
{
    for (size_t i = 0; i < prim->attributes_count; ++i)
    {
        cgltf_attribute const& attr = prim->attributes[i];
        if (attr.type == cgltf_attribute_type_custom && attr.name && std::strcmp(attr.name, name) == 0)
            return attr.data;
    }
    return nullptr;
}

bool IsLineCurvePrimitive(const cgltf_primitive* prim)
{
    if (!prim)
        return false;
    if (prim->type != cgltf_primitive_type_lines && prim->type != cgltf_primitive_type_line_strip &&
        prim->type != cgltf_primitive_type_line_loop)
        return false;
    return FindCustomAttribute(prim, "_RADIUS") != nullptr;
}

// Volume-compensated DOTS radius scale: 1 / (sin(pi/4) / (pi/4)).
static constexpr float kDOTSRadiusScale = 1.1107207345f;

void EmitDOTSLeaf(FImportedCurveSegment const& segment, Vector<FCurveDOTSVertex>& vertices, Vector<uint32_t>& indices,
                  Vector<FCurveLeaf>& leaves)
{
    float3 axis = segment.p1 - segment.p0;
    float len2 = dot(axis, axis);
    if (len2 <= 1e-12f || (segment.r0 <= 0.0f && segment.r1 <= 0.0f))
        return;

    float3 fwd = axis * (1.0f / std::sqrt(len2));
    float3 s, t;
    CoordinateSystem(fwd, s, t);
    float sr0 = segment.r0 * kDOTSRadiusScale;
    float sr1 = segment.r1 * kDOTSRadiusScale;

    leaves.push_back(FCurveLeaf{.p0 = segment.p0,
                                .r0 = segment.r0,
                                .p1 = segment.p1,
                                .r1 = segment.r1,
                                .u0 = segment.u0,
                                .u1 = segment.u1});

    float3 axes[2] = {s, t};
    for (float3 const& side : axes)
    {
        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back(FCurveDOTSVertex{.position = segment.p0 + side * sr0, .pad = 0.0f});
        vertices.push_back(FCurveDOTSVertex{.position = segment.p1 + side * sr1, .pad = 0.0f});
        vertices.push_back(FCurveDOTSVertex{.position = segment.p1 - side * sr1, .pad = 0.0f});
        vertices.push_back(FCurveDOTSVertex{.position = segment.p0 - side * sr0, .pad = 0.0f});
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

StringView Trim(StringView value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

bool ParseLUTTuple(StringView tuple, StringView expectedKind,
                          StringView& outView, StringView& outLook)
{
    size_t first = tuple.find(" / ");
    if (first == StringView::npos)
        return false;
    size_t second = tuple.find(" / ", first + 3);
    if (second == StringView::npos)
        return false;

    StringView kind = Trim(tuple.substr(0, first));
    if (kind != expectedKind)
        return false;

    outView = Trim(tuple.substr(first + 3, second - (first + 3)));
    outLook = Trim(tuple.substr(second + 3));
    return !outView.empty() && !outLook.empty();
}

struct FEnvironmentTextureSource
{
    Optional<String> path;
    cgltf_buffer_view const* bufferView{};

    [[nodiscard]] bool HasValue() const
    {
        return path.has_value() || bufferView != nullptr;
    }
};

void LoadFoundationColorManagementExtension(cgltf_data const* data, FSceneGlobals& result)
{
    if (!data->has_foundation_color_management)
        return;

    cgltf_foundation_color_management const& colorManagement = data->foundation_color_management;
    if (colorManagement.has_post_exposure)
        result.postExposure = colorManagement.post_exposure;

    StringView view;
    StringView look;
    if (colorManagement.sdr && ParseLUTTuple(colorManagement.sdr, "SDR", view, look))
        result.viewLutSdrIndex = Postprocess::MatchViewLUTIndex(Postprocess::ViewLUTDomain::SDR, view, look,
                                                                Postprocess::GetDefaultViewLUTIndex(Postprocess::ViewLUTDomain::SDR));
    if (colorManagement.hdr && ParseLUTTuple(colorManagement.hdr, "HDR", view, look))
        result.viewLutHdrIndex = Postprocess::MatchViewLUTIndex(Postprocess::ViewLUTDomain::HDR, view, look,
                                                                Postprocess::GetDefaultViewLUTIndex(Postprocess::ViewLUTDomain::HDR));
}

FEnvironmentTextureSource LoadFoundationEnvironmentExtension(cgltf_data const* data, StringView scenePath,
                                                            FLight& environmentLight)
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
        CHECK_MSG(environment.uri || environment.buffer_view, "EXT_foundation_environment HDRI requires uri or bufferView");

        environmentLight = MakeDefaultEnvironmentLight();
        environmentLight.power = environment.strength;
        environmentLight.environmentMap = true;
        environmentLight.environmentAzimuthOffset = environment.azimuth_offset;
        if (environment.buffer_view)
            return FEnvironmentTextureSource{.bufferView = environment.buffer_view};

        String uri = DecodeURI(environment.uri);
        std::filesystem::path hdriPath = std::filesystem::path(scenePath.data()).parent_path() / uri;
        return FEnvironmentTextureSource{.path = hdriPath.string()};
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

// Builds a membership set of table ids (non-nil, unique).
template <typename T>
HashSet<FUUID> BuildIdSet(Vector<T> const& vec, const char* what)
{
    HashSet<FUUID> ids(GLOBAL_ALLOC);
    ids.reserve(vec.size());
    for (size_t i = 0; i < vec.size(); ++i)
    {
        CHECK_MSG(!vec[i].id.IsNil(), "{}[{}] has a nil id", what, i);
        CHECK_MSG(ids.insert(vec[i].id).second, "{} has a duplicate id at index {}", what, i);
    }
    return ids;
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
    CHECK_MSG(tables.skeletons.size() <= UINT32_MAX, "FScene skeleton table is too large");
    CHECK_MSG(tables.clips.size() <= UINT32_MAX, "FScene animation clip table is too large");

    // String pool: content-addressed ids (id == hash of value).
    HashSet<FUUID> stringIds(GLOBAL_ALLOC);
    stringIds.reserve(tables.strings.size());
    for (size_t i = 0; i < tables.strings.size(); ++i)
    {
        FStringEntry const& e = tables.strings[i];
        CHECK_MSG(!e.id.IsNil(), "FScene string[{}] has a nil id", i);
        CHECK_MSG(e.id == FUUID::FromString(e.value),
                  "FScene string[{}] id is not the content hash of its value", i);
        CHECK_MSG(stringIds.insert(e.id).second, "FScene string has a duplicate id at index {}", i);
    }

    HashSet<FUUID> const materialIds = BuildIdSet(tables.materials, "FScene material");
    HashSet<FUUID> const meshIds = BuildIdSet(tables.meshes, "FScene mesh");
    HashSet<FUUID> const curveIds = BuildIdSet(tables.curves, "FScene curve");
    HashSet<FUUID> const textureIds = BuildIdSet(tables.textures, "FScene texture");
    HashSet<FUUID> const skeletonIds = BuildIdSet(tables.skeletons, "FScene skeleton");
    BuildIdSet(tables.clips, "FScene animation clip");
    BuildIdSet(tables.instances, "FScene instance");
    BuildIdSet(tables.cameras, "FScene camera");
    BuildIdSet(tables.lights, "FScene light");
    for (FSkeleton const& skeleton : tables.skeletons)
        for (uint32_t joint = 0; joint < skeleton.Count(); ++joint)
            CHECK_MSG(skeleton.joints[joint].parent >= -1 &&
                          skeleton.joints[joint].parent < static_cast<int32_t>(joint),
                      "Skeleton joints must be topologically sorted");

    auto requireTexture = [&](FUUID id, const char* what)
    {
        if (id.IsNil())
            return;
        CHECK_MSG(textureIds.contains(id), "{} references unknown texture id", what);
    };
    auto requireMaterial = [&](FUUID id, const char* what)
    {
        CHECK_MSG(materialIds.contains(id), "{} references unknown material id", what);
    };

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
                CHECK_MSG(!light.environmentTexture.IsNil(),
                          "FScene EnvMap environment light requires environmentTexture");
                requireTexture(light.environmentTexture, "light.environmentTexture");
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
        requireTexture(material.baseColorTexture, "material.baseColorTexture");
        requireTexture(material.emissiveTexture, "material.emissiveTexture");
        requireTexture(material.metallicRoughnessTexture, "material.metallicRoughnessTexture");
        requireTexture(material.normalTexture, "material.normalTexture");
        requireTexture(material.transmissionTexture, "material.transmissionTexture");
        requireTexture(material.specularTexture, "material.specularTexture");
        requireTexture(material.specularColorTexture, "material.specularColorTexture");
        requireTexture(material.anisotropyTexture, "material.anisotropyTexture");
        requireTexture(material.clearcoatTexture, "material.clearcoatTexture");
        requireTexture(material.clearcoatRoughnessTexture, "material.clearcoatRoughnessTexture");
    }

    for (auto const& instance : tables.instances)
    {
        requireMaterial(instance.material, "FScene instance material");
        switch (instance.type)
        {
        case FInstanceType::Mesh:
            CHECK_MSG(meshIds.contains(instance.resource), "FScene instance references unknown mesh id");
            break;
        case FInstanceType::Curve:
            CHECK_MSG(curveIds.contains(instance.resource), "FScene instance references unknown curve id");
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
        if (mesh.skeleton.IsNil())
        {
            CHECK_MSG(mesh.skinBinding.decodedSize == 0 && mesh.skinBinding.count == 0,
                      "Rigid mesh must not carry skin bindings");
        }
        else
        {
            CHECK_MSG(skeletonIds.contains(mesh.skeleton), "Skinned mesh references unknown skeleton id");
            ValidateBlobArray<FSkinBinding>(header, mesh.skinBinding, "mesh.skinBinding");
            CHECK_MSG(mesh.skinBinding.count == mesh.vertexCount, "Skinned mesh binding count mismatch");
        }
    }

    for (auto const& clip : tables.clips)
    {
        CHECK_MSG(skeletonIds.contains(clip.skeleton), "Animation clip references unknown skeleton id");
        CHECK_MSG(clip.duration >= 0.0f, "Animation clip has a negative duration");
        auto skeleton = std::find_if(tables.skeletons.begin(), tables.skeletons.end(),
                                     [&](FSkeleton const& value) { return value.id == clip.skeleton; });
        CHECK(skeleton != tables.skeletons.end());
        for (FAnimChannel const& channel : clip.channels)
        {
            CHECK_MSG(channel.joint < skeleton->Count(), "Animation channel references an invalid joint");
            uint32_t components = channel.path == FAnimPath::Rotation ? 4u : 3u;
            uint32_t multiplier = channel.interp == FAnimInterp::CubicSpline ? 3u : 1u;
            CHECK_MSG(channel.values.size() == channel.times.size() * components * multiplier,
                      "Animation channel key/value count mismatch");
            CHECK_MSG(std::is_sorted(channel.times.begin(), channel.times.end()),
                      "Animation channel times must be sorted");
        }
    }

    for (auto const& curve : tables.curves)
    {
        ValidateBlobArray<FCurveDOTSVertex>(header, curve.vertices, "curve.vertices");
        ValidateBlobArray<uint32_t>(header, curve.indices, "curve.indices");
        ValidateBlobArray<FCurveLeaf>(header, curve.leaves, "curve.leaves");
        CHECK_MSG(curve.indices.count % 3 == 0, "FScene curve index count must be a multiple of 3");
        CHECK_MSG(curve.indices.count == curve.leaves.count * 12,
                  "FScene curve DOTS index/leaf count mismatch: {} indices for {} leaves", curve.indices.count,
                  curve.leaves.count);
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
                             Vector<uint16_t> const* jointRemap = nullptr)
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

    // Skin binding (JOINTS_0/WEIGHTS_0). Joint indices are remapped into the skeleton's
    // topological order; vertices are left unoptimized so binding stays parallel to @ref vertices.
    if (const cgltf_accessor* jointsAcc = cgltf_find_accessor(submesh, cgltf_attribute_type_joints, 0))
    {
        const cgltf_accessor* weightsAcc = cgltf_find_accessor(submesh, cgltf_attribute_type_weights, 0);
        CHECK_MSG(weightsAcc, "JOINTS_0 present without WEIGHTS_0");
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
        String imageName = std::filesystem::path(uri).filename().string();
        std::filesystem::path dir = std::filesystem::path(scenePath.data()).parent_path();
        dir = dir / std::filesystem::path(uri).parent_path();
        auto fullpath = dir / imageName;
        auto result = LoadTexture(fullpath.string(), scratchAlloc, gamma);
        if (result)
            return result;
        LOG(Scene, LogWarn, "Texture image file not found: {}", uri);
        return {};
    }
    return {};
}

void LoadGLTFLineCurve(const cgltf_primitive* prim, FImportedCurve& curve, Allocator* scratchAlloc)
{
    CHECK(scratchAlloc != nullptr);
    CHECK(IsLineCurvePrimitive(prim));

    auto* positionAcc = cgltf_find_accessor(prim, cgltf_attribute_type_position, 0);
    auto* radiusAcc = FindCustomAttribute(prim, "_RADIUS");
    auto* texcoordAcc = cgltf_find_accessor(prim, cgltf_attribute_type_texcoord, 0);
    CHECK_MSG(positionAcc, "Curve LINES primitive missing POSITION");
    CHECK_MSG(radiusAcc, "Curve LINES primitive missing _RADIUS");
    CHECK_MSG(positionAcc->type == cgltf_type_vec3, "Curve POSITION must be VEC3");
    CHECK_MSG(radiusAcc->type == cgltf_type_scalar, "Curve _RADIUS must be SCALAR");

    size_t pointCount = positionAcc->count;
    CHECK_MSG(radiusAcc->count == pointCount, "Curve _RADIUS count ({}) != POSITION count ({})", radiusAcc->count,
              pointCount);

    Vector<float> positions(pointCount * 3, scratchAlloc);
    Vector<float> radii(pointCount, scratchAlloc);
    Vector<float> us(pointCount, 0.0f, scratchAlloc);
    cgltf_accessor_unpack_floats(positionAcc, positions.data(), positions.size());
    cgltf_accessor_unpack_floats(radiusAcc, radii.data(), radii.size());
    if (texcoordAcc)
    {
        CHECK_MSG(texcoordAcc->count == pointCount, "Curve TEXCOORD_0 count mismatch");
        Vector<float> uvs(pointCount * 2, scratchAlloc);
        cgltf_accessor_unpack_floats(texcoordAcc, uvs.data(), uvs.size());
        for (size_t i = 0; i < pointCount; ++i)
            us[i] = uvs[i * 2];
    }

    auto EmitSegment = [&](uint32_t i0, uint32_t i1)
    {
        CHECK_MSG(i0 < pointCount && i1 < pointCount, "Curve line index out of range");
        float3 p0{positions[i0 * 3 + 0], positions[i0 * 3 + 1], positions[i0 * 3 + 2]};
        float3 p1{positions[i1 * 3 + 0], positions[i1 * 3 + 1], positions[i1 * 3 + 2]};
        if (dot(p1 - p0, p1 - p0) <= 1e-12f)
            return;
        curve.segments.push_back(FImportedCurveSegment{.p0 = p0,
                                                       .r0 = std::max(radii[i0], 0.0f),
                                                       .p1 = p1,
                                                       .r1 = std::max(radii[i1], 0.0f),
                                                       .u0 = us[i0],
                                                       .u1 = us[i1]});
    };

    if (prim->type == cgltf_primitive_type_lines)
    {
        if (prim->indices)
        {
            size_t indexCount = prim->indices->count;
            CHECK_MSG(indexCount % 2 == 0, "Indexed LINES index count must be even");
            Vector<uint32_t> indices(indexCount, scratchAlloc);
            cgltf_accessor_unpack_indices(prim->indices, indices.data(), sizeof(uint32_t), indexCount);
            for (size_t i = 0; i + 1 < indexCount; i += 2)
                EmitSegment(indices[i], indices[i + 1]);
        }
        else
        {
            CHECK_MSG(pointCount % 2 == 0, "Non-indexed LINES vertex count must be even");
            for (size_t i = 0; i + 1 < pointCount; i += 2)
                EmitSegment(static_cast<uint32_t>(i), static_cast<uint32_t>(i + 1));
        }
    }
    else
    {
        Vector<uint32_t> order(scratchAlloc);
        if (prim->indices)
        {
            size_t indexCount = prim->indices->count;
            order.resize(indexCount);
            cgltf_accessor_unpack_indices(prim->indices, order.data(), sizeof(uint32_t), indexCount);
        }
        else
        {
            order.resize(pointCount);
            std::iota(order.begin(), order.end(), 0u);
        }
        CHECK_MSG(order.size() >= 2, "LINE_STRIP/LINE_LOOP requires at least two vertices");
        for (size_t i = 0; i + 1 < order.size(); ++i)
            EmitSegment(order[i], order[i + 1]);
        if (prim->type == cgltf_primitive_type_line_loop)
            EmitSegment(order.back(), order.front());
    }

    CHECK_MSG(!curve.segments.empty(), "Curve LINES primitive produced no valid segments");
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
void BuildMeshBlobJobs(FSerializedMesh& desc, Vector<FBlobJob>& blobJobs, FImportedMesh const& mesh,
                       FUUID skeleton = kNilUUID);
void BuildCurveBlobJobs(FSerializedCurve& desc, Vector<FBlobJob>& blobJobs, FImportedCurve const& curve);

// Flat, topologically sorted skeleton from a glTF skin. outRemap maps skin-local joint indices
// (as stored in JOINTS_0) to the sorted order; parents above the skin are treated as identity.
FSkeleton BuildSkeletonFromSkin(cgltf_data* data, cgltf_skin const* skin, Vector<uint16_t>& outRemap,
                                Allocator* alloc)
{
    FSkeleton skel(alloc);
    size_t n = skin->joints_count;
    outRemap.assign(n, 0);
    if (n == 0)
        return skel;

    Vector<int32_t> parentLocal(n, -1, alloc);
    for (size_t k = 0; k < n; k++)
    {
        if (cgltf_node* parent = skin->joints[k]->parent)
            for (size_t m = 0; m < n; m++)
                if (skin->joints[m] == parent)
                {
                    parentLocal[k] = static_cast<int32_t>(m);
                    break;
                }
    }
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
    case cgltf_interpolation_type_step: return FAnimInterp::Step;
    case cgltf_interpolation_type_cubic_spline: return FAnimInterp::CubicSpline;
    default: return FAnimInterp::Linear;
    }
}

bool MapAnimPath(cgltf_animation_path_type path, FAnimPath& out)
{
    switch (path)
    {
    case cgltf_animation_path_type_translation: out = FAnimPath::Translation; return true;
    case cgltf_animation_path_type_rotation: out = FAnimPath::Rotation; return true;
    case cgltf_animation_path_type_scale: out = FAnimPath::Scale; return true;
    default: return false; // morph weights unsupported
    }
}

bool BuildAnimChannel(cgltf_animation_channel const* ch, uint32_t joint, FAnimChannel& out, float& duration)
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

void BuildGLTFSerializedScene(StringView path, FImportedScene& scene, Allocator* scratchAlloc,
                              FSceneBuildOptions const& buildOptions)
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
    FEnvironmentTextureSource environmentTextureSource = LoadFoundationEnvironmentExtension(data, path, environmentLight);

    /* Texture ids are reserved up front so materials can reference textures before payloads load. */
    CHECK_MSG(data->textures_count <= UINT32_MAX, "glTF texture count exceeds uint32_t");
    CHECK_MSG(!environmentTextureSource.HasValue() || data->textures_count < UINT32_MAX,
              "glTF texture count leaves no room for environment texture");
    size_t const sceneTextureCount = data->textures_count + (environmentTextureSource.HasValue() ? 1u : 0u);
    uint32_t const environmentTextureIndex = environmentTextureSource.HasValue()
        ? static_cast<uint32_t>(data->textures_count)
        : kInvalidTexture;
    scene.mTables.textures.clear();
    scene.mTables.textures.reserve(sceneTextureCount);
    for (size_t i = 0; i < sceneTextureCount; ++i)
    {
        scene.mTables.textures.emplace_back(scratchAlloc);
        scene.mTables.textures.back().id = FUUID::Generate();
    }

    /* Materials — slot 0 is kDefaultMaterialUUID (glTF default material). */
    Vector<FUUID> gltfMaterialIds(data->materials_count, kNilUUID, scratchAlloc);
    // Main-thread only: worker jobs below must not touch the string pool.
    auto internString = [&](const char* s) -> FUUID { return scene.InternString(s); };
    // A glTF animation's display name; a stable fallback keeps its skin + rigid clips grouped and
    // gives the UI something to select even when the source animation is unnamed.
    auto animName = [&](const cgltf_animation* anim, size_t index) -> FUUID
    {
        if (anim->name && *anim->name)
            return scene.InternString(anim->name);
        return scene.InternString(fmt::format("Animation {}", index).c_str());
    };
    // glTF animation pointer -> interned name id, for resolving EXT_foundation_animation strips.
    HashMap<cgltf_animation const*, FUUID> animationName(scratchAlloc);
    scene.Add(FMaterial{
        .id = kDefaultMaterialUUID,
        .baseColorFactor = {1.0f, 1.0f, 1.0f, 1.0f},
    });
    // Extra texture flags. Mostly used for sRGB to linear conversion
    constexpr unsigned kTextureInSRGB = 1 << 0;
    Vector<unsigned> textureFlags(data->textures_count, 0, scratchAlloc);
    auto assignTextureId = [&](cgltf_texture_view const& view, unsigned flags = 0u) -> FUUID
    {
        if (!view.texture)
            return kNilUUID;
        size_t index = cgltf_texture_index(data, view.texture);
        CHECK_MSG(index < data->textures_count, "glTF texture index out of range");
        textureFlags[index] |= flags;
        return scene.mTables.textures[index].id;
    };
    for (size_t i = 0; i < data->materials_count; i++)
    {
        const cgltf_material* mat = &data->materials[i];
        FMaterial material{};
        material.id = FUUID::Generate();
        material.name = internString(mat->name);
        gltfMaterialIds[i] = material.id;
        if (mat->has_pbr_metallic_roughness)
        {
            material.baseColorFactor = {
                mat->pbr_metallic_roughness.base_color_factor[0], mat->pbr_metallic_roughness.base_color_factor[1],
                mat->pbr_metallic_roughness.base_color_factor[2], mat->pbr_metallic_roughness.base_color_factor[3]};
            material.metallicFactor = mat->pbr_metallic_roughness.metallic_factor;
            material.roughnessFactor = mat->pbr_metallic_roughness.roughness_factor;
            if (mat->pbr_metallic_roughness.base_color_texture.texture)
                material.baseColorTexture = assignTextureId(mat->pbr_metallic_roughness.base_color_texture, kTextureInSRGB);
            if (mat->pbr_metallic_roughness.metallic_roughness_texture.texture)
                material.metallicRoughnessTexture = assignTextureId(mat->pbr_metallic_roughness.metallic_roughness_texture);
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
                material.baseColorTexture = assignTextureId(sg.diffuse_texture, kTextureInSRGB);
        }
        material.normalScale = mat->normal_texture.scale;
        if (mat->normal_texture.texture)
            material.normalTexture = assignTextureId(mat->normal_texture);
        if (mat->emissive_texture.texture)
            material.emissiveTexture = assignTextureId(mat->emissive_texture, kTextureInSRGB);
        material.emissiveFactor = {mat->emissive_factor[0], mat->emissive_factor[1], mat->emissive_factor[2], 1.0f};
        if (mat->emissive_strength.emissive_strength)
            material.emissiveFactor *= mat->emissive_strength.emissive_strength;
        material.transmissionFactor = mat->has_transmission ? mat->transmission.transmission_factor : 0.0f;
        if (mat->has_transmission && mat->transmission.transmission_texture.texture)
            material.transmissionTexture = assignTextureId(mat->transmission.transmission_texture);
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
                material.specularTexture = assignTextureId(mat->specular.specular_texture);
            if (mat->specular.specular_color_texture.texture)
                material.specularColorTexture = assignTextureId(mat->specular.specular_color_texture, kTextureInSRGB);
        }
        if (mat->has_anisotropy)
        {
            material.anisotropyStrength = std::clamp(mat->anisotropy.anisotropy_strength, 0.0f, 1.0f);
            material.anisotropyRotation = mat->anisotropy.anisotropy_rotation;
            if (mat->anisotropy.anisotropy_texture.texture)
                material.anisotropyTexture = assignTextureId(mat->anisotropy.anisotropy_texture);
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
                material.sheenColorTexture = assignTextureId(mat->sheen.sheen_color_texture, kTextureInSRGB);
            if (mat->sheen.sheen_roughness_texture.texture)
                material.sheenRoughnessTexture = assignTextureId(mat->sheen.sheen_roughness_texture);
        }
        if (mat->has_clearcoat)
        {
            material.clearcoatFactor = std::clamp(mat->clearcoat.clearcoat_factor, 0.0f, 1.0f);
            material.clearcoatRoughnessFactor = std::clamp(mat->clearcoat.clearcoat_roughness_factor, 0.0f, 1.0f);
            if (mat->clearcoat.clearcoat_texture.texture)
                material.clearcoatTexture = assignTextureId(mat->clearcoat.clearcoat_texture);
            if (mat->clearcoat.clearcoat_roughness_texture.texture)
                material.clearcoatRoughnessTexture = assignTextureId(mat->clearcoat.clearcoat_roughness_texture);
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
        LoadFoundationMaterialExtension(mat, material);
        scene.Add(material);
    }

    Vector<FBlobJob> blobJobs(scratchAlloc);

    /* Textures */
    FTexture textureCodecInit(scratchAlloc);
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
            // Name interning happens on the main thread (the pool must not mutate the string pool).
            scene.mTables.textures[i].name = internString(data->textures[i].name);
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
                            if (buildOptions.generateMipmaps)
                                loaded->GenerateMips();
                            if (buildOptions.textureCompression == FSceneTextureCompression::BC7)
                                texture = loaded->EncodeBC7(scratchAlloc);
                            else
                                texture = std::move(loaded.value());
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
    if (environmentTextureSource.HasValue())
    {
        CHECK_MSG(environmentTextureIndex != kInvalidTexture, "Invalid environment texture index");
        FTexture environmentTexture(scratchAlloc);
        if (environmentTextureSource.bufferView)
        {
            cgltf_buffer_view const* view = environmentTextureSource.bufferView;
            CHECK_MSG(view->buffer && view->buffer->data, "Embedded environment HDRI buffer is not loaded");
            CHECK_MSG(view->offset <= view->buffer->size && view->size <= view->buffer->size - view->offset,
                      "Embedded environment HDRI bufferView is out of range");
            LOG(Scene, LogInfo, "Loading embedded environment HDRI ({} bytes)", view->size);
            Span<const unsigned char> imgData{
                static_cast<const unsigned char*>(view->buffer->data) + view->offset,
                view->size};
            LoadHDR(environmentTexture, imgData);
        }
        else
        {
            LOG(Scene, LogInfo, "Loading environment HDRI {}", *environmentTextureSource.path);
            auto result = LoadTexture(*environmentTextureSource.path, scratchAlloc, true);
            if (!result)
            {
                LOG(Scene, LogError, "Failed to load environment HDRI {}", *environmentTextureSource.path);
                return;
            }
            environmentTexture = std::move(result.value());
        }
        BuildTextureBlobJobs(scene.mTables.textures[environmentTextureIndex],
                             textureBlobJobs[environmentTextureIndex].jobs, std::move(environmentTexture));
        AppendResourceBlobJobs(blobJobs, textureBlobJobs[environmentTextureIndex]);
        environmentLight.environmentTexture = scene.mTables.textures[environmentTextureIndex].id;
    }

    /* Meshes (triangles) and curves (LINES + _RADIUS) */
    struct MeshPrimitiveResource
    {
        FInstanceType type{FInstanceType::Mesh};
        uint32_t index{~0u}; // into meshes or curves table
    };
    size_t numTrianglePrimitives = 0;
    size_t numCurvePrimitives = 0;
    for (size_t i = 0; i < data->meshes_count; i++)
    {
        for (size_t p = 0; p < data->meshes[i].primitives_count; ++p)
        {
            auto* sub = data->meshes[i].primitives + p;
            if (sub->type == cgltf_primitive_type_triangles)
                ++numTrianglePrimitives;
            else if (IsLineCurvePrimitive(sub))
                ++numCurvePrimitives;
            else
                CHECK_MSG(false, "Unsupported glTF primitive mode {} (only triangles and LINES+_RADIUS curves)",
                          static_cast<int>(sub->type));
        }
    }

    scene.mTables.meshes.reserve(numTrianglePrimitives);
    for (size_t i = 0; i < numTrianglePrimitives; i++)
        scene.mTables.meshes.emplace_back(scratchAlloc);
    for (size_t i = 0; i < numTrianglePrimitives; i++)
        scene.mTables.meshes[i].id = FUUID::Generate();

    scene.mTables.curves.resize(numCurvePrimitives);
    for (size_t i = 0; i < numCurvePrimitives; i++)
        scene.mTables.curves[i].id = FUUID::Generate();

    Vector<FResourceBlobJobs> meshBlobJobs(scratchAlloc);
    meshBlobJobs.reserve(numTrianglePrimitives);
    for (size_t i = 0; i < numTrianglePrimitives; i++)
        meshBlobJobs.emplace_back(scratchAlloc);
    Vector<FResourceBlobJobs> curveBlobJobs(scratchAlloc);
    curveBlobJobs.reserve(numCurvePrimitives);
    for (size_t i = 0; i < numCurvePrimitives; i++)
        curveBlobJobs.emplace_back(scratchAlloc);

    Vector<Vector<MeshPrimitiveResource>> meshPrimitiveResources(scratchAlloc);
    meshPrimitiveResources.reserve(data->meshes_count);
    for (size_t i = 0; i < data->meshes_count; ++i)
        meshPrimitiveResources.push_back(Vector<MeshPrimitiveResource>(scratchAlloc));

    // Skinning: one topologically sorted skeleton per glTF skin, plus a joint remap and a
    // mesh->skin map so each skinned submesh can carry its skeleton id and remapped bindings.
    Vector<Vector<uint16_t>> skinRemap(scratchAlloc);
    skinRemap.reserve(data->skins_count);
    scene.mTables.skeletons.reserve(data->skins_count);
    Vector<FUUID> skinSkeletonIds(data->skins_count, kNilUUID, scratchAlloc);
    for (size_t s = 0; s < data->skins_count; s++)
    {
        Vector<uint16_t> remap(scratchAlloc);
        FSkeleton skel = BuildSkeletonFromSkin(data, &data->skins[s], remap, scratchAlloc);
        skel.id = FUUID::Generate();
        skinSkeletonIds[s] = skel.id;
        scene.mTables.skeletons.push_back(std::move(skel));
        skinRemap.push_back(std::move(remap));
    }
    Vector<int32_t> meshToSkin(data->meshes_count, -1, scratchAlloc);
    for (size_t i = 0; i < data->nodes_count; i++)
    {
        cgltf_node const* node = &data->nodes[i];
        if (node->mesh && node->skin)
            meshToSkin[cgltf_mesh_index(data, node->mesh)] = static_cast<int32_t>(cgltf_skin_index(data, node->skin));
    }

    uint32_t nextSubmesh = 0;
    uint32_t nextCurve = 0;
    size_t totalPrimitives = numTrianglePrimitives + numCurvePrimitives;
    if (totalPrimitives != 0)
    {
        ThreadPool pool(GetSceneWorkerCount(), GetSceneTaskQueueSize(totalPrimitives), scratchAlloc, "SceneGeo");
        Vector<Future<void>> futures(scratchAlloc);
        futures.reserve(totalPrimitives);
        for (size_t i = 0; i < data->meshes_count; i++)
        {
            auto& mesh = data->meshes[i];
            int32_t const skinIndex = meshToSkin[i];
            Vector<uint16_t> const* const remap = skinIndex >= 0 ? &skinRemap[skinIndex] : nullptr;
            FUUID const skeletonId = skinIndex >= 0 ? skinSkeletonIds[skinIndex] : kNilUUID;
            meshPrimitiveResources[i].reserve(mesh.primitives_count);
            for (size_t p = 0; p < mesh.primitives_count; p++)
            {
                auto* sub = mesh.primitives + p;
                if (sub->type == cgltf_primitive_type_triangles)
                {
                    uint32_t meshIndex = nextSubmesh++;
                    meshPrimitiveResources[i].push_back(
                        MeshPrimitiveResource{.type = FInstanceType::Mesh, .index = meshIndex});
                    futures.push_back(pool.Push(
                        [&, meshIndex, sub, remap, skeletonId]
                        {
                            FImportedMesh submesh = LoadGLTFSubmesh(sub, scratchAlloc, remap);
                            // Skinned vertices must keep their original order so skin bindings stay
                            // parallel; vertex-fetch/cache optimization would desync them.
                            if (submesh.skin.empty() && buildOptions.optimizeMeshes)
                            {
                                LOG(Meshopt, LogInfo, "Optimizing submesh {}, vtx: {}, idx: {}", meshIndex,
                                    submesh.vertices.size(), submesh.lods[0].indices.size());
                                submesh.Optimize();
                            }
                            if (buildOptions.generateMeshlets)
                            {
                                LOG(Meshopt, LogInfo, "Clusterizing submesh {}, vtx: {}, idx: {}", meshIndex,
                                    submesh.vertices.size(), submesh.lods[0].indices.size());
                                submesh.ClusterizeDAG();
                            }
                            submesh.Quantize();
                            BuildMeshBlobJobs(scene.mTables.meshes[meshIndex], meshBlobJobs[meshIndex].jobs, submesh,
                                              submesh.skin.empty() ? kNilUUID : skeletonId);
                        }));
                }
                else
                {
                    uint32_t curveIndex = nextCurve++;
                    FUUID curveId = scene.mTables.curves[curveIndex].id;
                    meshPrimitiveResources[i].push_back(
                        MeshPrimitiveResource{.type = FInstanceType::Curve, .index = curveIndex});
                    futures.push_back(pool.Push(
                        [&, curveIndex, curveId, sub]
                        {
                            FImportedCurve curve(scratchAlloc);
                            LoadGLTFLineCurve(sub, curve, scratchAlloc);
                            BuildCurveBlobJobs(scene.mTables.curves[curveIndex], curveBlobJobs[curveIndex].jobs, curve);
                            scene.mTables.curves[curveIndex].id = curveId;
                        }));
                }
            }
        }
        pool.Join();
        for (Future<void>& future : futures)
            future.get();
        for (FResourceBlobJobs& jobs : meshBlobJobs)
            AppendResourceBlobJobs(blobJobs, jobs);
        for (FResourceBlobJobs& jobs : curveBlobJobs)
            AppendResourceBlobJobs(blobJobs, jobs);
    }
    CHECK_MSG(nextSubmesh == numTrianglePrimitives, "Serialized mesh count mismatch");
    CHECK_MSG(nextCurve == numCurvePrimitives, "Serialized curve count mismatch");

    /* Skin clips: one per glTF animation that targets a skinned joint, bound to the first skin a
     * channel targets. Channels for other skins or unsupported paths (morph weights) are dropped. */
    auto findSkinJoint = [&](cgltf_node* node, int32_t& outSkin, uint32_t& outJoint) -> bool
    {
        for (size_t s = 0; s < data->skins_count; s++)
            for (size_t l = 0; l < data->skins[s].joints_count; l++)
                if (data->skins[s].joints[l] == node)
                {
                    outSkin = static_cast<int32_t>(s);
                    outJoint = skinRemap[s][l];
                    return true;
                }
        return false;
    };
    for (size_t a = 0; a < data->animations_count; a++)
    {
        cgltf_animation const* anim = &data->animations[a];
        int32_t clipSkin = -1;
        for (size_t c = 0; c < anim->channels_count && clipSkin < 0; c++)
        {
            int32_t s;
            uint32_t j;
            if (anim->channels[c].target_node && findSkinJoint(anim->channels[c].target_node, s, j))
                clipSkin = s;
        }
        if (clipSkin < 0)
            continue;

        FAnimationClip clip(scratchAlloc);
        clip.id = FUUID::Generate();
        clip.name = animName(anim, a);
        animationName[anim] = clip.name;
        clip.skeleton = skinSkeletonIds[clipSkin];
        for (size_t c = 0; c < anim->channels_count; c++)
        {
            cgltf_animation_channel const* ch = &anim->channels[c];
            int32_t s;
            uint32_t j;
            if (!ch->target_node || !findSkinJoint(ch->target_node, s, j) || s != clipSkin)
                continue;
            FAnimChannel channel(scratchAlloc);
            if (!BuildAnimChannel(ch, j, channel, clip.duration))
                continue;
            clip.channels.push_back(std::move(channel));
        }
        if (!clip.channels.empty())
            scene.mTables.clips.push_back(std::move(clip));
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
            mat4 worldMat;
            cgltf_node_transform_world(node, reinterpret_cast<float*>(&worldMat));

            auto meshIndex = cgltf_mesh_index(data, node->mesh);
            auto const& primResources = meshPrimitiveResources[meshIndex];

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
            instance.name = internString(node->name);

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
                for (size_t p = 0; p < primResources.size(); ++p)
                {
                    auto* sub = node->mesh->primitives + p;
                    auto const& resource = primResources[p];
                    instance.id = FUUID::Generate();
                    instance.type = resource.type;
                    if (resource.type == FInstanceType::Curve)
                        instance.resource = scene.mTables.curves[resource.index].id;
                    else
                        instance.resource = scene.mTables.meshes[resource.index].id;
                    instance.material = sub->material ? gltfMaterialIds[cgltf_material_index(data, sub->material)]
                                                       : kDefaultMaterialUUID;
                    scene.Add(instance);
                }
            }
        }
        if (node->camera)
        {
            FCamera camera{};
            camera.id = FUUID::Generate();
            getTransform(camera.transform);
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
            light.id = FUUID::Generate();
            light.name = internString(node->light->name);
            getTransform(light.transform);
            light.color = float3{node->light->color[0], node->light->color[1], node->light->color[2]};
            light.power = node->light->intensity / 683.0f;
            light.useShadow = node->light->has_foundation_lights ? node->light->use_shadow != 0 : true;
            switch (node->light->type)
            {
            case cgltf_light_type_directional:
                light.type = FLightType::Directional;
                light.angularDiameter = node->light->has_foundation_lights ? node->light->angular_diameter : 0.0f;
                break;
            case cgltf_light_type_point:
                light.type = FLightType::Point;
                light.radius = node->light->has_foundation_lights ? std::max(node->light->radius, 0.0f) : 0.0f;
                break;
            case cgltf_light_type_spot:
                light.type = FLightType::Spot;
                light.radius = node->light->has_foundation_lights ? std::max(node->light->radius, 0.0f) : 0.0f;
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
            light.id = FUUID::Generate();
            light.name = internString(node->light_area->name);
            getTransform(light.transform);
            float ws = std::max({std::abs(light.transform.scale.x), std::abs(light.transform.scale.y),
                                 std::abs(light.transform.scale.z)});
            light.transform.scale = float3{1, 1, 1};

            const cgltf_light_area* la = node->light_area;
            light.color = float3{la->color[0], la->color[1], la->color[2]};
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

void BuildDOTSGeometry(FImportedCurve const& curve, Vector<FCurveDOTSVertex>& vertices, Vector<uint32_t>& indices,
                       Vector<FCurveLeaf>& leaves)
{
    vertices.clear();
    indices.clear();
    leaves.clear();
    vertices.reserve(curve.segments.size() * 8);
    indices.reserve(curve.segments.size() * 12);
    leaves.reserve(curve.segments.size());
    for (FImportedCurveSegment const& segment : curve.segments)
        EmitDOTSLeaf(segment, vertices, indices, leaves);
    CHECK_MSG(!leaves.empty(), "DOTS bake produced no leaves");
    CHECK_MSG(indices.size() == leaves.size() * 12, "DOTS index/leaf count mismatch");
}

void BuildMeshBlobJobs(FSerializedMesh& desc, Vector<FBlobJob>& blobJobs, FImportedMesh const& mesh, FUUID skeleton)
{
    CHECK_MSG(!mesh.verticesQuantized.empty(), "FScene mesh is not quantized");
    desc.bounds = BuildMeshBounds(mesh);
    desc.vertices = {};
    desc.vertexCount = static_cast<uint32_t>(mesh.verticesQuantized.size());
    desc.lods.clear();
    desc.dagGroups = {};
    desc.dagMeshlets = {};
    desc.dagMeshletTri = {};
    desc.dagMeshletVtx = {};
    desc.skinBinding = {};
    desc.skeleton = skeleton;

    AppendArrayBlobJob(blobJobs, mesh.verticesQuantized, FBlobCodec::LZ4, desc.vertices);
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
    AppendArrayBlobJob(blobJobs, mesh.skin, FBlobCodec::LZ4, desc.skinBinding);
}

void BuildCurveBlobJobs(FSerializedCurve& desc, Vector<FBlobJob>& blobJobs, FImportedCurve const& curve)
{
    desc = {};
    desc.bounds = BuildCurveBounds(curve);

    Allocator* scratchAlloc = blobJobs.get_allocator().mResource;
    Vector<FCurveDOTSVertex> vertices(scratchAlloc);
    Vector<uint32_t> indices(scratchAlloc);
    Vector<FCurveLeaf> leaves(scratchAlloc);
    BuildDOTSGeometry(curve, vertices, indices, leaves);

    AppendArrayBlobJob(blobJobs, vertices, FBlobCodec::LZ4, desc.vertices);
    AppendArrayBlobJob(blobJobs, indices, FBlobCodec::LZ4, desc.indices);
    AppendArrayBlobJob(blobJobs, leaves, FBlobCodec::LZ4, desc.leaves);
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

void FImportedScene::RebuildIndex()
{
    auto fill = [](HashMap<FUUID, uint32_t>& map, auto const& vec)
    {
        map.clear();
        map.reserve(vec.size());
        for (uint32_t i = 0; i < static_cast<uint32_t>(vec.size()); ++i)
            map.emplace(vec[i].id, i);
    };
    fill(mIndex.strings, mTables.strings);
    fill(mIndex.instances, mTables.instances);
    fill(mIndex.materials, mTables.materials);
    fill(mIndex.lights, mTables.lights);
    fill(mIndex.textures, mTables.textures);
    fill(mIndex.meshes, mTables.meshes);
    fill(mIndex.curves, mTables.curves);
    fill(mIndex.skeletons, mTables.skeletons);
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
    size_t rigidMeshCount = 0;
    for (auto const& mesh : GetMeshes())
    {
        if (!mesh.skeleton.IsNil())
            continue;
        primitiveBytes = AlignUp(primitiveBytes, size_t(4));
        primitiveBytes += GPUScene::CalculateMeshPrimitiveSize(mesh);
        ++rigidMeshCount;
    }
    for (auto const& curve : GetCurves())
    {
        primitiveBytes = AlignUp(primitiveBytes, size_t(4));
        primitiveBytes += GPUScene::CalculateCurvePrimitiveSize(curve);
    }

    desc.primitiveBudget = ByteGPUSceneBudget(primitiveBytes, desc.primitiveBudget, size_t(4));

    size_t intersectableLightCount = 0;
    for (auto const& light : GetLights())
    {
        if (light.type == FLightType::Disk || light.type == FLightType::Rect ||
            ((light.type == FLightType::Point || light.type == FLightType::Spot) && light.radius > 0.0f))
            intersectableLightCount++;
    }
    const size_t tlasInstanceCount = GetInstances().size() + intersectableLightCount;
    desc.instanceBudget = RingGPUSceneBudget(GetInstances().size());
    desc.tlasInstanceBudget = RingGPUSceneBudget(tlasInstanceCount);
    desc.materialBudget = RingGPUSceneBudget(GetMaterials().size());
    desc.lightBudget = RingGPUSceneBudget(GetLights().size());
    size_t skinnedInstanceCount = 0;
    size_t dynamicBytes = 0;
    for (FInstance const& instance : GetInstances())
    {
        if (instance.type != FInstanceType::Mesh)
            continue;
        int meshIndex = MeshIndex(instance.resource);
        if (meshIndex < 0)
            continue;
        FSerializedMesh const& mesh = GetMeshes()[static_cast<size_t>(meshIndex)];
        if (mesh.skeleton.IsNil())
            continue;
        CHECK_MSG(!mesh.lods.empty(), "Skinned mesh has no LOD0");
        uint64_t bytes = sizeof(GSMesh) + static_cast<uint64_t>(mesh.vertexCount) * sizeof(FQVertex) +
            static_cast<uint64_t>(mesh.lods[0].indexCount) * sizeof(uint32_t);
        dynamicBytes += static_cast<size_t>(AlignUp(bytes, 16ull));
        ++skinnedInstanceCount;
    }
    desc.dynamicGeometryBudget = ByteGPUSceneBudget(dynamicBytes, 0, size_t(16));
    desc.geometryBudget =
        CountGPUSceneBudget(rigidMeshCount + GetCurves().size() + skinnedInstanceCount);
    desc.dynamicStagingFramesInFlight = kGPUSceneRingFrameSlack;

    size_t textureBindings = kGPUScenePersistentTexture2DBindings + kGPUSceneDefaultTextureBindings +
        kGPUSceneTextureBindingSlack;
    FLight const* environmentLight = GetEnvironmentLight();
    for (size_t textureIndex = 0; textureIndex < GetTextures().size(); ++textureIndex)
    {
        if (environmentLight && environmentLight->HasEnvironmentTexture() &&
            GetTextures()[textureIndex].id == environmentLight->environmentTexture)
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

void LoadGLTF(StringView path, FImportedScene& scene, Allocator* scratchAlloc,
              FSceneBuildOptions const& buildOptions)
{
    CHECK(scene.mWriting);
    CHECK(scratchAlloc != nullptr);
    BuildGLTFSerializedScene(path, scene, scratchAlloc, buildOptions);
    scene.RebuildIndex();
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
    scene.RebuildIndex();
}

String LoadScene(StringView path, FImportedScene& scene, Allocator* scratchAlloc,
                 FSceneBuildOptions const& buildOptions)
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
    LoadGLTF(path, scene, scratchAlloc, buildOptions);
    return String(path);
}