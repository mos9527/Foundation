#include "GPUScene.hpp"
#include <Renderer/Renderer.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

bool IsSceneEnvironmentTexture(FImportedScene const& scene, size_t textureIndex)
{
    FLight const* environment = scene.GetEnvironmentLight();
    return environment != nullptr && environment->HasEnvironmentTexture() &&
           textureIndex < scene.GetTextures().size() &&
           scene.GetTextures()[textureIndex].id == environment->environmentTexture;
}

GPUSceneDesc CalculateSceneGPUDesc(FImportedScene const& scene, Foundation::RHI::RHIDeviceCapabilities const& caps,
                                   uint32_t minLightBudget)
{
    GPUSceneDesc desc = scene.CalculateGPUSceneDesc(caps);
    desc.lightBudget = std::max(desc.lightBudget, minLightBudget);
    return desc;
}

void UploadSceneResources(FImportedScene& scene, GPUScene& gpu, FSceneGPUResources& outResources)
{
    FBlobDeserializer blobs = scene.GetBlobDeserializer();

    outResources.meshGeometry.clear();
    outResources.curveGeometry.clear();
    outResources.meshGeometry.reserve(scene.GetMeshes().size());
    outResources.curveGeometry.reserve(scene.GetCurves().size());
    outResources.meshById.clear();
    outResources.curveById.clear();
    outResources.textureById.clear();
    outResources.materialById.clear();
    for (FSerializedMesh const& mesh : scene.GetMeshes())
    {
        GeometryHandle handle;
        GPUScene::Result r = gpu.Upload(&blobs, mesh, handle);
        CHECK_MSG(r == GPUScene::Result::InProgress || r == GPUScene::Result::Ready, "Mesh upload rejected ({})",
                  static_cast<int>(r));
        outResources.meshGeometry.push_back(handle);
        outResources.meshById.emplace(mesh.id, static_cast<uint32_t>(outResources.meshGeometry.size() - 1));
    }
    for (FSerializedCurve const& curve : scene.GetCurves())
    {
        GeometryHandle handle;
        GPUScene::Result r = gpu.Upload(&blobs, curve, handle);
        CHECK_MSG(r == GPUScene::Result::InProgress, "Curve upload rejected ({})", static_cast<int>(r));
        outResources.curveGeometry.push_back(handle);
        outResources.curveById.emplace(curve.id, static_cast<uint32_t>(outResources.curveGeometry.size() - 1));
    }

    outResources.textureIDMap.assign(scene.GetTextures().size(), TextureHandle{});
    for (size_t textureIndex = 0; textureIndex < scene.GetTextures().size(); ++textureIndex)
    {
        FSerializedTexture const& srcDesc = scene.GetTextures()[textureIndex];
        outResources.textureById.emplace(srcDesc.id, static_cast<uint32_t>(textureIndex));
        if (!srcDesc.IsValid() || IsSceneEnvironmentTexture(scene, textureIndex))
            continue;
        TextureHandle handle;
        GPUScene::Result r = gpu.Upload(&blobs, srcDesc, handle);
        CHECK_MSG(r == GPUScene::Result::InProgress, "Texture {} upload rejected ({})", textureIndex,
                  static_cast<int>(r));
        outResources.textureIDMap[textureIndex] = handle;
    }

    auto const& materials = scene.GetMaterials();
    for (size_t i = 0; i < materials.size(); ++i)
        outResources.materialById.emplace(materials[i].id, static_cast<uint32_t>(i));
}

namespace
{
size_t SceneTextureReadBudget(FSerializedTexture const& source)
{
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");
    size_t budget = static_cast<size_t>(source.GetSize());
    for (FBlobRef const& blob : source.subresources)
        if (blob.codec != FBlobCodec::None)
            budget += static_cast<size_t>(blob.decodedSize);
    return std::max<size_t>(AlignUp(budget + (1ull << 20), alignof(std::max_align_t)), alignof(std::max_align_t));
}

FTexture ReadSceneTexture(FImportedScene const& scene, FSerializedTexture const& source, Allocator* alloc)
{
    CHECK(alloc != nullptr);
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");

    FTexture texture(alloc);
    texture.magic = source.magic;
    texture.header = source.header;
    texture.header10 = source.header10;
    texture.bytes.resize(texture.GetSize());
    for (uint32_t layer = 0; layer < source.GetNumLayers(); ++layer)
    {
        for (uint32_t mip = 0; mip < source.GetNumMips(); ++mip)
        {
            Span<unsigned char> dst = texture.GetSubresource(mip, layer);
            FBlobRef const& blob = source.GetSubresourceBlob(layer, mip);
            CHECK_MSG(blob.decodedSize == dst.size_bytes(),
                      "Serialized texture subresource size mismatch: layer {}, mip {}, blob {}, expected {}", layer,
                      mip, blob.decodedSize, dst.size_bytes());
            CHECK(scene.ReadBlob(blob, dst.data(), dst.size_bytes(), alloc));
        }
    }
    return texture;
}
} // namespace

void UploadSceneEnvironment(FImportedScene const& scene, FLight const& environment, GPUScene& gpu)
{
    CHECK_MSG(environment.type == FLightType::Environment && environment.HasEnvironmentTexture(),
              "UploadSceneEnvironment requires an Environment light with an environment map texture");
    int const envTexIndex = scene.TextureIndex(environment.environmentTexture);
    CHECK_MSG(envTexIndex >= 0, "Scene environment texture id not found");
    FSerializedTexture const& environmentTextureDesc = scene.GetTextures()[static_cast<size_t>(envTexIndex)];
    ScopedArena environmentArena(GLOBAL_ALLOC, SceneTextureReadBudget(environmentTextureDesc));
    CHECK(environmentArena);
    AllocatorStack environmentAlloc(environmentArena);
    FTexture environmentTexture = ReadSceneTexture(scene, environmentTextureDesc, &environmentAlloc);
    CHECK_MSG(environmentTexture.GetFormat() == RHIResourceFormat::R32G32B32A32SignedFloat,
              "Scene environment texture must be RGBA32F, got {}", environmentTexture.GetFormat());
    GPUScene::Result r = gpu.UploadEnvironmentMap(environmentTexture);
    CHECK_MSG(r == GPUScene::Result::Ready, "Environment map upload rejected ({})", static_cast<int>(r));
}

namespace
{
void FLightToGSLight(FLight const& src, GSLight& dst, GPUScene const& gpu)
{
    dst = GSLight{};
    dst.flags = static_cast<uint32_t>(src.type);
    if (src.twoSided)
        dst.flags |= kGSLightFlagTwoSided;
    if (src.useShadow)
        dst.flags |= kGSLightFlagUseShadow;
    dst.color = src.color;
    dst.power = src.power;
    dst.position = src.transform.transform;
    // Default forward is (0,0,-1).
    dst.direction = normalize(src.transform.rotation * float3(0, 0, -1));
    float areaWidth = std::max(src.width, 1e-6f);
    float areaHeight = std::max(src.height, 1e-6f);
    if (src.type == FLightType::Directional)
        dst.params.x = src.angularDiameter;
    else if (src.type == FLightType::Point)
        dst.params.x = std::max(src.radius, 0.0f);
    else if (src.type == FLightType::Spot)
        dst.params = float4(std::max(src.radius, 0.0f), std::cos(src.spotInnerConeAngle),
                            std::cos(src.spotOuterConeAngle), 0.0f);
    else if (src.type == FLightType::Disk)
        dst.params = float4(areaWidth, areaHeight, 0.0f, 0.0f);
    if (src.type == FLightType::Disk || src.type == FLightType::Rect)
    {
        float3 n = dst.direction;
        float3 u, v;
        CoordinateSystem(n, u, v);
        if (src.type == FLightType::Disk)
        {
            dst.dpdu = u;
            dst.dpdv = v;
        }
        else
        {
            dst.dpdu = u * areaWidth;
            dst.dpdv = v * areaHeight;
        }

        if (src.normalize)
        {
            float area = src.type == FLightType::Disk ? std::numbers::pi_v<float> * areaWidth * areaHeight
                                                        : 4.0f * areaWidth * areaHeight;
            float totalArea = src.twoSided ? 2.0f * area : area;
            dst.power = src.power / (totalArea * std::numbers::pi_v<float>);
        }
    }
    if (src.type == FLightType::Environment)
    {
        bool const hasEnvMap = src.environmentMap && gpu.HasEnvMap();
        dst.color = hasEnvMap ? float3(1.0f) : src.color;
        dst.power = src.power;
        if (hasEnvMap)
            dst.flags |= kGSLightFlagEnvironmentMap;
        dst.params.x = src.environmentAzimuthOffset;
    }
}

void FillGSMaterial(GSMaterial& dst, FMaterial const& src, FSceneGPUResources const& resources,
                    GPUScene const& gpu)
{
    // Nil/unready textures -> UINT32_MAX (shader default).
    auto resolveTexture = [&](FUUID id) -> uint32_t
    {
        if (id.IsNil())
            return UINT32_MAX;
        auto it = resources.textureById.find(id);
        if (it == resources.textureById.end())
            return UINT32_MAX;
        if (it->second >= resources.textureIDMap.size())
            return UINT32_MAX;
        TextureHandle const handle = resources.textureIDMap[it->second];
        if (!handle.IsValid() || gpu.Query(handle) != GPUScene::Result::Ready)
            return UINT32_MAX;
        return handle.index;
    };
    dst.baseColorFactor = src.baseColorFactor;
    // emissiveFactor.w is the emissive intensity multiplier; bake it into the GPU RGB.
    dst.emissiveFactor = float3(src.emissiveFactor) * src.emissiveFactor.w;
    dst.metallicFactor = src.metallicFactor;
    dst.roughnessFactor = src.roughnessFactor;
    dst.baseColorTexture = resolveTexture(src.baseColorTexture);
    dst.emissiveTexture = resolveTexture(src.emissiveTexture);
    dst.metallicRoughnessTexture = resolveTexture(src.metallicRoughnessTexture);
    dst.normalTexture = resolveTexture(src.normalTexture);
    dst.normalScale = src.normalScale;
    dst.transmissionTexture = resolveTexture(src.transmissionTexture);
    dst.specularTexture = resolveTexture(src.specularTexture);
    dst.specularColorTexture = resolveTexture(src.specularColorTexture);
    dst.anisotropyTexture = resolveTexture(src.anisotropyTexture);
    dst.sheenColorTexture = resolveTexture(src.sheenColorTexture);
    dst.sheenRoughnessTexture = resolveTexture(src.sheenRoughnessTexture);
    dst.clearcoatTexture = resolveTexture(src.clearcoatTexture);
    dst.clearcoatRoughnessTexture = resolveTexture(src.clearcoatRoughnessTexture);
    dst.transmissionFactor = src.transmissionFactor;
    dst.ior = src.ior;
    dst.specularFactor = src.specularFactor;
    dst.specularColorFactor = src.specularColorFactor;
    dst.anisotropyStrength = src.anisotropyStrength;
    dst.anisotropyRotation = src.anisotropyRotation;
    dst.sheenColorFactor = src.sheenColorFactor;
    dst.sheenRoughnessFactor = src.sheenRoughnessFactor;
    dst.clearcoatFactor = src.clearcoatFactor;
    dst.clearcoatRoughnessFactor = src.clearcoatRoughnessFactor;
    dst.subsurfaceFactor = src.subsurfaceFactor;
    dst.subsurfaceScale = src.subsurfaceScale;
    dst.subsurfaceColor = src.subsurfaceColor;
    dst.subsurfaceRadius = src.subsurfaceRadius;
    dst.shaderBlockID = static_cast<uint32_t>(src.shaderBlockID);
    dst.hairBetaM = src.hairBetaM;
    dst.hairBetaN = src.hairBetaN;
    dst.hairAlpha = src.hairAlpha;
}
} // namespace

GPUScene::UpdateResult CommitSceneToGPU(FImportedScene& scene, GPUScene& gpu, FSceneGPUResources const& resources,
                                        RendererUBO& globals, uint32_t frameNumber)
{
    auto instances = scene.GetInstances();
    auto materials = scene.GetMaterials();
    scene.EnsureEnvironmentLight();
    auto lights = scene.GetLights();
    CHECK_MSG(instances.size() <= UINT32_MAX && materials.size() <= UINT32_MAX && lights.size() <= UINT32_MAX,
              "Scene table exceeds uint32_t range");
    auto tables = gpu.BeginScene(static_cast<uint32_t>(instances.size()), static_cast<uint32_t>(materials.size()),
                                 static_cast<uint32_t>(lights.size()));
    for (size_t i = 0; i < instances.size(); ++i)
    {
        auto const& src = instances[i];
        GeometryHandle geometry;
        if (src.type == FInstanceType::Mesh)
        {
            auto it = resources.meshById.find(src.resource);
            CHECK_MSG(it != resources.meshById.end(), "Mesh instance references unknown mesh id");
            CHECK_MSG(it->second < resources.meshGeometry.size(), "Mesh instance references invalid mesh {}",
                      it->second);
            geometry = resources.meshGeometry[it->second];
        }
        else if (src.type == FInstanceType::Curve)
        {
            auto it = resources.curveById.find(src.resource);
            CHECK_MSG(it != resources.curveById.end(), "Curve instance references unknown curve id");
            CHECK_MSG(it->second < resources.curveGeometry.size(),
                      "Curve instance references invalid curve {}", it->second);
            geometry = resources.curveGeometry[it->second];
        }
        else
            CHECK_MSG(false, "Unknown scene instance type {}", static_cast<uint32_t>(src.type));
        auto matIt = resources.materialById.find(src.material);
        CHECK_MSG(matIt != resources.materialById.end(), "Instance references unknown material id");
        tables.instances[i] = {            
            .transform = src.transform.transform,
            .rotation = src.transform.rotation,
            .scale = src.transform.scale,
            .materialIndex = matIt->second,
            .resourceIndex = geometry.index
            // ^^ Assign only resource index *here* so later EndScene resolves it            
        };
    }
    for (size_t i = 0; i < materials.size(); ++i)
        FillGSMaterial(tables.materials[i], materials[i], resources, gpu);
    for (size_t i = 0; i < lights.size(); ++i)
        FLightToGSLight(lights[i], tables.lights[i], gpu);

    uint32_t const motionFrame = frameNumber != UINT32_MAX ? frameNumber : globals.frameNumber;
    GPUScene::UpdateResult result = gpu.EndScene(tables, motionFrame);
    gpu.UpdateUBO(globals);
    return result;
}
