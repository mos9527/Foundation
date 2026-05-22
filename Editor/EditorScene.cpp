#include <Core/Paths.hpp>
#include <filesystem>
#include <limits>
#include <numbers>

#include "EditorState.hpp"
#include "Scene/Mesh.hpp"

static constexpr const char* kTempScenePath = "Cache/Last.fscn";
static constexpr size_t kDefaultSceneLoadScratchBudget = 64ull * (1ull << 20);
// Accounts for alignment, etc...
static constexpr size_t kBudgetSlack = 1ull * (1ull << 20);
static constexpr size_t kStagingBudgetSlack = 32ull * (1ull << 20);

static GPUScene::UpdateResult CommitSceneToGPU(GPUScene* gpu, Span<GSInstance> instances,
                                                Span<GSMaterial> materials, Span<GSLight> lights,
                                                UBO& globals, bool resetAccumulation)
{
    CHECK(gpu);

    auto res = gpu->UpdateGPUScene(instances, materials, lights);
    globals.firstInstance = res.firstInstance;
    globals.numInstances  = res.numInstances;
    globals.firstMaterial = res.firstMaterial;
    globals.numMaterials  = res.numMaterials;
    globals.firstLight    = res.firstLight;
    globals.firstLightAliasTable = res.firstLightAliasTable;
    globals.sceneLightWeightSum = res.sceneLightWeightSum;
    if (resetAccumulation)
        globals.ptAccumulatedFrames = 0;
    return res;
}

void CommitSceneToGPU(bool resetAccumulation)
{
    if (!GContext->gpuScene)
        return;

    CommitSceneToGPU(GContext->gpuScene, GEditor.instances, GEditor.materials, GEditor.lights,
                     GEditor.shaderGlobals, resetAccumulation);
}

static FTexture LoadViewLUT(StringView path, Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    FTexture texture(alloc);
    LoadDDS(texture, PathsResolve(path));
    return texture;
}

static String ResolveSelectedViewLUTPath(int& index, ViewLUTEntry const* entries, int count,
                                         int defaultIndex, String const& externalPath)
{
    const int externalIndex = count;
    if (index == externalIndex && !externalPath.empty())
        return externalPath;

    if (index < 0 || index >= count)
        index = std::clamp(defaultIndex, 0, count - 1);
    return entries[index].path;
}

static void LoadSelectedViewLUTs(FTexture& sdr, FTexture& hdr, int& sdrIndex, int& hdrIndex,
                                 String const& sdrExternalPath, String const& hdrExternalPath,
                                 Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    String sdrPath = ResolveSelectedViewLUTPath(sdrIndex, kViewLUTsSdr, kViewLUTSdrCount,
                                                kDefaultViewLUTSdr, sdrExternalPath);
    String hdrPath = ResolveSelectedViewLUTPath(hdrIndex, kViewLUTsHdr, kViewLUTHdrCount,
                                                kDefaultViewLUTHdr, hdrExternalPath);

    sdr = LoadViewLUT(sdrPath, alloc);
    hdr = LoadViewLUT(hdrPath, alloc);
}

static void LoadSelectedViewLUTs(FTexture& sdr, FTexture& hdr)
{
    LoadSelectedViewLUTs(sdr, hdr, GEditor.viewLUTSdrIndex, GEditor.viewLUTHdrIndex,
                         GEditor.viewLUTSdrExternalPath, GEditor.viewLUTHdrExternalPath);
}

static double MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

static size_t TextureSubresourceUploadStagingFootprint(FTextureHeader const& metadata,
                                                      uint32_t layer, uint32_t mip)
{
    uint32_t const alignment = std::max(metadata.GetBpp() / 8, metadata.GetBlockSize());
    CHECK_MSG(alignment != 0, "Unsupported texture format {}", metadata.GetFormat());
    size_t const size = metadata.GetSubresourceSize(layer, mip);
    CHECK_MSG(size <= std::numeric_limits<size_t>::max() - (alignment - 1u),
              "Texture subresource staging footprint exceeds addressable range");
    return size + alignment - 1u;
}

static bool IsSceneEnvironmentTexture(FImportedScene const& scene, size_t textureIndex)
{
    auto const& environment = scene.GetSceneGlobals();
    return environment.type == FSceneEnvironmentType::EnvMap &&
        environment.environmentTexture != kInvalidTexture &&
        textureIndex == environment.environmentTexture;
}

// Max memory required for uploading the scene to the GPU
static size_t SceneStagingBufferBudget(FImportedScene const& scene, bool directGeometryUpload)
{
    size_t budget = 0ull;
    if (!directGeometryUpload)
    {
        // These will be decompressed if we'd do it directly into the mapped GPU mem
        // In which case an intermediate decompression to CPU would be required to not thrash the (non-existent) GPU cache
        for (FSerializedMesh const& mesh : scene.GetMeshes())
            budget = std::max(budget, GPUScene::CalculateMeshPrimitiveSize(mesh) + kBudgetSlack);
        for (FSerializedCurve const& curve : scene.GetCurves())
            budget = std::max(budget, GPUScene::CalculateCurvePrimitiveSize(curve) +
                                      GPUScene::CalculateCurveAABBSize(curve) + kBudgetSlack);
    }
    for (size_t textureIndex = 0; textureIndex < scene.GetTextures().size(); ++textureIndex)
    {
        FSerializedTexture const& texture = scene.GetTextures()[textureIndex];
        if (!texture.IsValid() || IsSceneEnvironmentTexture(scene, textureIndex))
            continue;
        for (uint32_t layer = 0; layer < texture.GetNumLayers(); ++layer)
            for (uint32_t mip = 0; mip < texture.GetNumMips(); ++mip)
                budget = std::max(budget, TextureSubresourceUploadStagingFootprint(texture, layer, mip) + kBudgetSlack);
    }

    auto const& environment = scene.GetSceneGlobals();
    if (environment.type == FSceneEnvironmentType::EnvMap && environment.environmentTexture != kInvalidTexture)
    {
        CHECK_MSG(environment.environmentTexture < scene.GetTextures().size(),
                  "Scene environment texture index out of range");
        FSerializedTexture const& texture = scene.GetTextures()[environment.environmentTexture];
        CHECK_MSG(texture.IsValid(), "Scene environment texture is invalid");
        const size_t envMapBytes = static_cast<size_t>(texture.GetSize());
        const size_t conditionalCDFBytes = static_cast<size_t>(texture.GetWidth()) * texture.GetHeight() * sizeof(float);
        const size_t marginalCDFBytes = static_cast<size_t>(texture.GetHeight()) * sizeof(float);
        budget = std::max(budget, envMapBytes + conditionalCDFBytes + marginalCDFBytes + kBudgetSlack);
    }
    return budget + kStagingBudgetSlack;
}

static size_t SceneBlobScratchBudget(FImportedScene const& scene)
{
    size_t budget = 0ull;
    auto IncludeBlob = [&](FBlobRef const& blob)
    {
        if (blob.codec != FBlobCodec::None)
            budget = std::max(budget, static_cast<size_t>(blob.decodedSize));
    };

    for (FSerializedMesh const& mesh : scene.GetMeshes())
    {
        IncludeBlob(mesh.vertices);
        if (!mesh.lods.empty())
            IncludeBlob(mesh.lods[0].indices);
        IncludeBlob(mesh.dagGroups);
        IncludeBlob(mesh.dagMeshlets);
        IncludeBlob(mesh.dagMeshletVtx);
        IncludeBlob(mesh.dagMeshletTri);
    }
    for (FSerializedCurve const& curve : scene.GetCurves())
    {
        IncludeBlob(curve.points);
        IncludeBlob(curve.segments);
        IncludeBlob(curve.aabbs);
    }
    for (size_t textureIndex = 0; textureIndex < scene.GetTextures().size(); ++textureIndex)
    {
        if (IsSceneEnvironmentTexture(scene, textureIndex))
            continue;
        FSerializedTexture const& texture = scene.GetTextures()[textureIndex];
        for (FBlobRef const& blob : texture.subresources)
            IncludeBlob(blob);
    }
    if (budget == 0)
        return alignof(std::max_align_t);
    return std::max<size_t>(AlignUp(budget + kBudgetSlack, alignof(std::max_align_t)), alignof(std::max_align_t));
}

static size_t EnvMapUploadStagingBudget(FTexture const& source)
{
    const size_t envMapBytes = static_cast<size_t>(source.GetSize());
    const size_t conditionalCDFBytes = static_cast<size_t>(source.GetWidth()) * source.GetHeight() * sizeof(float);
    const size_t marginalCDFBytes = static_cast<size_t>(source.GetHeight()) * sizeof(float);
    return envMapBytes + conditionalCDFBytes + marginalCDFBytes + kBudgetSlack;
}

static size_t SceneTextureReadBudget(FSerializedTexture const& source)
{
    CHECK_MSG(source.IsValid(), "Serialized texture is invalid");
    size_t budget = static_cast<size_t>(source.GetSize());
    for (FBlobRef const& blob : source.subresources)
    {
        if (blob.codec != FBlobCodec::None)
            budget += static_cast<size_t>(blob.decodedSize);
    }
    return std::max<size_t>(AlignUp(budget + kBudgetSlack, alignof(std::max_align_t)), alignof(std::max_align_t));
}

static FTexture ReadSceneTexture(FImportedScene const& scene, FSerializedTexture const& source, Allocator* alloc)
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
                      "Serialized texture subresource size mismatch: layer {}, mip {}, blob {}, expected {}",
                      layer, mip, blob.decodedSize, dst.size_bytes());
            CHECK(scene.ReadBlob(blob, dst.data(), dst.size_bytes(), alloc));
        }
    }
    return texture;
}

bool ApplyViewLUTSelection()
{
    if (!GContext->gpuScene)
    {
        LOG(Editor, LogWarn, "Ignoring view LUT selection before a scene is loaded");
        return false;
    }
    try
    {
        FTexture sdr(GLOBAL_ALLOC);
        FTexture hdr(GLOBAL_ALLOC);
        LoadSelectedViewLUTs(sdr, hdr);

        const size_t uploadBudget = static_cast<size_t>(sdr.GetSize()) + static_cast<size_t>(hdr.GetSize()) +
                                    (1u << 20);
        ImmediateUpload upload(GContext->device.Get(), uploadBudget);
        upload.Begin();
        GContext->gpuScene->UploadViewLUTs(&upload, sdr, hdr);
        upload.End();
        upload.WaitIdle();
    }
    catch (std::exception const& e)
    {
        LOG(Editor, LogError, "Failed to apply view LUT selection: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to apply view LUT selection");
        return false;
    }

    GEditor.shaderGlobals.viewLutIndex = GContext->enableHDR
        ? GContext->gpuScene->GetViewLutHdrIndex()
        : GContext->gpuScene->GetViewLutSdrIndex();
    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
    return true;
}

FTexture ReadbackRenderTexture(RHITexture* source, Allocator* alloc)
{
    CHECK(source != nullptr);
    CHECK(alloc != nullptr);
    RHITextureDesc const& desc = source->mDesc;
    CHECK_MSG(desc.format != RHIResourceFormat::Undefined, "Cannot read back texture with undefined format");
    CHECK_MSG(desc.sampleCount == RHIMultisampleCount::E1, "Cannot read back multisampled texture");

    FTexture texture(alloc);
    texture.Initialize(desc.format, desc.dimension, desc.extent.x, desc.extent.y, desc.extent.z,
                       desc.mipLevels, desc.arrayLayers);
    texture.bytes.resize(texture.GetSize());

    uint32_t const alignment = std::max(texture.GetBpp() / 8, texture.GetBlockSize());
    CHECK_MSG(alignment != 0, "Unsupported texture format {}", texture.GetFormat());
    CHECK_MSG(texture.bytes.size() <= std::numeric_limits<size_t>::max() - (alignment - 1u),
              "Texture readback staging capacity exceeds addressable range");
    size_t const readbackCapacity = texture.bytes.size() + alignment - 1u;

    ImmediateReadback readback(GContext->device.Get(), readbackCapacity);
    readback.Begin();
    auto* cmd = readback.ctx.Get();
    auto range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0, desc.mipLevels,
                                                    0, desc.arrayLayers);
    cmd->BeginTransition();
    cmd->SetImageTransition(source,
                            {.srcAccess = RHIResourceAccessBits::ShaderRead,
                             .dstAccess = RHIResourceAccessBits::TransferRead,
                             .srcStage = RHIPipelineStageBits::FragmentShader,
                             .dstStage = RHIPipelineStageBits::Transfer,
                             .srcImgLayout = RHITextureLayout::ShaderReadOnly,
                             .dstImgLayout = RHITextureLayout::TransferSrc,
                             .srcImgRange = range});
    cmd->EndTransition();

    struct ReadbackSlice
    {
        char* data;
        size_t offset;
        size_t size;
    };
    Vector<ReadbackSlice> slices(alloc);
    slices.reserve(static_cast<size_t>(desc.arrayLayers) * desc.mipLevels);
    for (uint32_t layer = 0; layer < desc.arrayLayers; ++layer)
    {
        for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
        {
            if (!readback.Align(alignment))
                CHECK_MSG(false, "Readback staging alignment failed");
            Span<unsigned char> subresource = texture.GetSubresource(mip, layer);
            RHIExtent3D mipExtent = texture.GetMipExtent(mip);
            char* data = readback.Readback(source, subresource.size(),
                                           {.aspect = RHITextureAspectFlagBits::Color,
                                            .mipLevel = mip,
                                            .baseArrayLayer = layer,
                                            .layerCount = 1},
                                           {}, mipExtent);
            CHECK_MSG(data != nullptr, "Readback staging buffer exhausted");
            slices.push_back({data, static_cast<size_t>(subresource.data() - texture.bytes.data()), subresource.size()});
        }
    }

    cmd->BeginTransition();
    cmd->SetImageTransition(source,
                            {.srcAccess = RHIResourceAccessBits::TransferRead,
                             .dstAccess = RHIResourceAccessBits::ShaderRead,
                             .srcStage = RHIPipelineStageBits::Transfer,
                             .dstStage = RHIPipelineStageBits::FragmentShader,
                             .srcImgLayout = RHITextureLayout::TransferSrc,
                             .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                             .srcImgRange = range});
    cmd->EndTransition();
    readback.End();
    readback.WaitIdle();

    for (ReadbackSlice const& slice : slices)
        std::memcpy(texture.bytes.data() + slice.offset, slice.data, slice.size);
    return texture;
}

static void ValidateRenderReadbackTexture(FTexture const& texture, RHIResourceFormat expectedFormat,
                                          uint32_t& outWidth, uint32_t& outHeight)
{
    CHECK(texture.IsValid());
    CHECK_MSG(texture.GetFormat() == expectedFormat,
              "Render readback expects {}, got {}", expectedFormat, texture.GetFormat());
    outWidth = texture.GetWidth();
    outHeight = texture.GetHeight();
}

static Vector<float> CombineRenderTextures(Span<const FTexture> textures, uint32_t& outWidth, uint32_t& outHeight,
                                         Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    CHECK_MSG(!textures.empty(), "Invalid render texture count");
    ValidateRenderReadbackTexture(textures[0], RHIResourceFormat::R32G32B32A32SignedFloat, outWidth, outHeight);
    for (FTexture const& texture : textures)
    {
        uint32_t width = 0;
        uint32_t height = 0;
        ValidateRenderReadbackTexture(texture, RHIResourceFormat::R32G32B32A32SignedFloat, width, height);
        CHECK_MSG(width == outWidth && height == outHeight, "Mismatched render readback texture extents");
    }
    size_t const pixelCount = static_cast<size_t>(outWidth) * outHeight;
    size_t const componentCount = pixelCount * 4;
    Vector<float> combined(componentCount, alloc);
    for (FTexture const& texture : textures)
    {
        auto const* rgba = reinterpret_cast<const float*>(texture.bytes.data());
        for (size_t i = 0; i < componentCount; ++i)
            combined[i] += rgba[i];
    }
    for (size_t i = 0; i < pixelCount; ++i)
        combined[i * 4 + 3] = 1.0f;
    return combined;
}

void DoRenderReadback(RendererHandles const& handles)
{
    auto* renderer = GContext->renderer;
    uint32_t w = 0;
    uint32_t h = 0;

    if (GEditor.renderTask.format == ERenderFormat::HDR)
    {
        CHECK_MSG(handles.numHdrRT, "Invalid HDR readback texture count");
        Vector<FTexture> hdrTextures(GLOBAL_ALLOC);
        hdrTextures.reserve(handles.numHdrRT);
        for (uint32_t i = 0; i < handles.numHdrRT; ++i)
        {
            auto* texture = renderer->DerefResource(handles.hdrRT[i]).Get<RHITexture*>();
            hdrTextures.push_back(ReadbackRenderTexture(texture, GLOBAL_ALLOC));
        }
        // AOVs are provided (without alpha), combine them into one HDR buffer
        const Vector<float> combined = CombineRenderTextures(hdrTextures, w, h);
        const char* hdrPath = GEditor.renderTask.outputPath.c_str();
        SaveHDR(combined.data(), static_cast<int>(w), static_cast<int>(h), hdrPath);
        LOG(Editor, LogInfo, "{} HDR image saved to {} ({}x{}, {} frames)",
            GEditor.rendererMode == ERendererMode::PathTracer ? "Path tracer" : "Raster", hdrPath, w, h,
            GEditor.shaderGlobals.ptAccumulatedFrames);
    } else
    {
        CHECK_MSG(handles.sdrRT != kInvalidHandle, "Invalid SDR readback texture");
        auto sdrTexture = renderer->DerefResource(handles.sdrRT).Get<RHITexture*>();
        const FTexture sdr = ReadbackRenderTexture(sdrTexture, GLOBAL_ALLOC);
        ValidateRenderReadbackTexture(sdr, RHIResourceFormat::R8G8B8A8Unorm, w, h);
        const char* sdrPath = GEditor.renderTask.outputPath.c_str();
        // Output as is
        SavePNG(sdr.bytes.data(), static_cast<int>(w), static_cast<int>(h), sdrPath);
        LOG(Editor, LogInfo, "{} SDR image saved to {} ({}x{}, {} frames)",
            GEditor.rendererMode == ERendererMode::PathTracer ? "Path tracer" : "Raster", sdrPath, w, h,
            GEditor.shaderGlobals.ptAccumulatedFrames);
    }
}

static void FLightToGSLight(FLight const& src, GSLight& dst, GPUScene::LightSamplerType sampler)
{
    dst.type = static_cast<uint32_t>(src.type);
    dst.color = src.color;
    dst.power = src.power;
    dst.range = src.range;
    // Position from transform
    dst.position = src.transform.transform;
    // Direction from rotation (default forward is (0,0,-1))
    dst.direction = normalize(src.transform.rotation * float3(0, 0, -1));
    dst.spotInnerCosAngle = std::cos(src.spotInnerConeAngle);
    dst.spotOuterCosAngle = std::cos(src.spotOuterConeAngle);
    // Area light fields
    float areaWidth = std::max(src.width, 1e-6f);
    float areaHeight = std::max(src.height, 1e-6f);
    dst.radius = float2(areaWidth, areaHeight);
    dst.twoSided = src.twoSided ? 1u : 0u;
    // Build tangent frame from direction for area lights
    if (src.type == FLightType::Disk || src.type == FLightType::Rect)
    {
        float3 n = dst.direction;
        float3 u, v;
        buildOrthonormalBasis(n, u, v);
        if (src.type == FLightType::Disk)
        {
            dst.dpdu = u; // Unit tangent; radius is separate
            dst.dpdv = v;
        }
        else // Rect
        {
            dst.dpdu = u * areaWidth;  // half-extent along u
            dst.dpdv = v * areaHeight; // half-extent along v
        }

        if (src.normalize)
        {
            float area;
            if (src.type == FLightType::Disk)
                area = std::numbers::pi_v<float> * areaWidth * areaHeight;
            else
                area = 4.0f * areaWidth * areaHeight;

            float totalArea = src.twoSided ? (2.0f * area) : area;
            // For a Lambertian emitter, total flux Phi = L * A * pi per emitting side.
            dst.power = src.power / (totalArea * std::numbers::pi_v<float>);
        }
    }
    // Selection weight
    float weight = 1.0f;
    if (sampler == GPUScene::LightSamplerType::Power) {
        weight = dst.power;
        if (dst.type == 3)
            weight *= dst.radius.x * dst.radius.y * pi<float>() * std::numbers::pi_v<float> *
                      (dst.twoSided != 0 ? 2.0f : 1.0f);
        else if (dst.type == 4)
            weight *= cross(dst.dpdu, dst.dpdv).length() * 4.0f * std::numbers::pi_v<float> *
                      (dst.twoSided != 0 ? 2.0f : 1.0f);
    }
    dst.selectionWeight = std::max(0.0f, weight);
}

static void BuildSceneLights(FImportedScene& scene, Vector<GSLight>& dstLights, UBO& globals,
                             GPUScene::LightSamplerType lightSamplerType)
{
    auto lights = scene.GetLights();
    CHECK_MSG(lights.size() <= UINT32_MAX, "Too many scene lights");
    uint32_t count = static_cast<uint32_t>(lights.size());
    globals.numSceneLights = count;

    dstLights.clear();
    dstLights.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        auto& src = lights[i];
        FLightToGSLight(src, dstLights[i], lightSamplerType);
    }
}

void UpdateSceneLights()
{
    if (!GEditor.HasScene())
        return;

    auto lightSamplerType = GContext->gpuScene
        ? GContext->gpuScene->mLightSamplerType
        : GPUScene::LightSamplerType::Power;
    BuildSceneLights(GEditor.Scene(), GEditor.lights, GEditor.shaderGlobals, lightSamplerType);

    CommitSceneToGPU(false);
}

static void BuildEditorMaterials(FImportedScene& scene, Vector<GSMaterial>& dstMaterials,
                                 Vector<uint32_t> const& textureIDMap)
{
    auto RemapTextureIndex = [&textureIDMap](uint32_t index) {
        if (index == UINT32_MAX)
            return UINT32_MAX;
        return textureIDMap[index];
    };
    dstMaterials.clear();
    for (auto& src : scene.GetMaterials())
    {
        auto& dst = dstMaterials.emplace_back();
        dst.baseColorFactor = src.baseColorFactor;
        dst.emissiveFactor = src.emissiveFactor;
        dst.metallicFactor = src.metallicFactor;
        dst.roughnessFactor = src.roughnessFactor;
        dst.baseColorTexture = RemapTextureIndex(src.baseColorTexture);
        dst.emissiveTexture = RemapTextureIndex(src.emissiveTexture);
        dst.metallicRoughnessTexture = RemapTextureIndex(src.metallicRoughnessTexture);
        dst.normalTexture = RemapTextureIndex(src.normalTexture);
        dst.transmissionTexture = RemapTextureIndex(src.transmissionTexture);
        dst.specularTexture = RemapTextureIndex(src.specularTexture);
        dst.specularColorTexture = RemapTextureIndex(src.specularColorTexture);
        dst.anisotropyTexture = RemapTextureIndex(src.anisotropyTexture);
        dst.sheenColorTexture = RemapTextureIndex(src.sheenColorTexture);
        dst.sheenRoughnessTexture = RemapTextureIndex(src.sheenRoughnessTexture);
        dst.clearcoatTexture = RemapTextureIndex(src.clearcoatTexture);
        dst.clearcoatRoughnessTexture = RemapTextureIndex(src.clearcoatRoughnessTexture);
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
}

static void BuildEditorInstances(FImportedScene& scene, Vector<GSInstance>& dstInstances,
                                 Vector<uint32_t> const& meshOffsets, Vector<uint32_t> const& curveOffsets)
{
    auto instances = scene.GetInstances();
    CHECK_MSG(instances.size() <= UINT32_MAX, "Too many scene instances");
    dstInstances.clear();
    for (size_t i = 0; i < instances.size(); i++)
    {
        auto const& src = instances[i];
        if (src.type == FInstanceType::Mesh)
        {
            CHECK_MSG(src.resourceIndex < meshOffsets.size(), "Mesh instance references invalid mesh {}", src.resourceIndex);
            auto& dst = dstInstances.emplace_back();
            dst.transform = src.transform.transform;
            dst.rotation = src.transform.rotation;
            dst.scale = src.transform.scale;
            dst.resourceOffset = meshOffsets[src.resourceIndex];
            dst.materialIndex = src.materialIndex;
            dst.resourceIndex = src.resourceIndex;
            dst.type = kGSInstanceTypeMesh;
        }
        else if (src.type == FInstanceType::Curve)
        {
            CHECK_MSG(src.resourceIndex < curveOffsets.size(), "Curve instance references invalid curve {}", src.resourceIndex);
            auto& dst = dstInstances.emplace_back();
            dst.transform = src.transform.transform;
            dst.rotation = src.transform.rotation;
            dst.scale = src.transform.scale;
            dst.resourceOffset = curveOffsets[src.resourceIndex];
            dst.materialIndex = src.materialIndex;
            dst.resourceIndex = src.resourceIndex;
            dst.type = kGSInstanceTypeCurve;
        }
        else
        {
            CHECK_MSG(false, "Unknown scene instance type {}", static_cast<uint32_t>(src.type));
        }
    }
}

static void ApplySceneCamera(FImportedScene const& scene, FArcballCamera& cameraState,
                             CameraApertureState& apertureState, UBO& globals)
{
    auto cameras = scene.GetCameras();
    if (cameras.empty())
        return;

    auto& camera = cameras.front();
    vec3 dir = camera.transform.rotation * vec3(0, 0, 1);
    cameraState.center = camera.transform.transform - dir * cameraState.radius;
    cameraState.rot = camera.transform.rotation;
    cameraState.fovY = camera.fovY;
    apertureState.dofEnabled = camera.lensEnabled;
    if (camera.lensEnabled)
    {
        apertureState.sensorHeightMm = camera.sensorHeightMm;
        apertureState.fStop = camera.fStop;
        globals.focalDistance = camera.focusDistance;
        globals.apertureBlades = camera.apertureBlades;
        globals.apertureRotation = camera.apertureRotation;
        globals.apertureRatio = camera.apertureRatio;
    }
}

static size_t GetSceneUploadTaskQueueSize(size_t taskCount)
{
    return ThreadPool::getTaskSize(std::max<size_t>(taskCount, 1u));
}

static size_t GPUSceneBudgetBytes(GPUScene::GPUSceneDesc const& desc)
{
    return size_t(desc.primitiveBudget) +
           size_t(desc.curveAABBBudget) +
           size_t(desc.instanceBudget) * sizeof(GSInstance) +
           size_t(desc.materialBudget) * sizeof(GSMaterial) +
           size_t(desc.lightBudget) * sizeof(GSLight) +
           size_t(desc.lightBudget) * sizeof(Alias) +
           size_t(desc.tlasInstanceBudget) * GContext->device->WriteAccelerationStructureInstanceData({}, nullptr) +
           size_t(desc.tlasBudget) +
           size_t(desc.tlasScratchBudget);
}

static GPUScene* CreateGPUScene(FImportedScene const& scene, size_t& outBudgetBytes)
{
    auto estimatedBudget = GPUScene::CalculateSceneBudget(scene, GContext->device->GetCapabilities());
    LOG(Editor, LogDebug,
        "Estimated GPUScene budget: primitive {} MB, curve AABB {} MB, instances {}, TLAS instances {}, materials {}, lights {}, textures {}",
        estimatedBudget.primitiveBudget / (1u << 20),
        estimatedBudget.curveAABBBudget / (1u << 20),
        estimatedBudget.instanceBudget,
        estimatedBudget.tlasInstanceBudget,
        estimatedBudget.materialBudget,
        estimatedBudget.lightBudget,
        estimatedBudget.texturesBudget);

    outBudgetBytes = GPUSceneBudgetBytes(estimatedBudget);

    auto lightSamplerType = GPUScene::LightSamplerType::Power;
    if (GContext->gpuScene)
        lightSamplerType = GContext->gpuScene->mLightSamplerType;

    auto* gpu = Construct<GPUScene>(GContext->allocator, GContext, estimatedBudget);
    gpu->mLightSamplerType = lightSamplerType;
    return gpu;
}

static void DestroyGPUScene(GPUScene*& gpu)
{
    if (!gpu)
        return;
    if (gpu == GContext->gpuScene)
        ClearMaterialTexturePreviewCache();
    Destruct(GContext->allocator, gpu);
    gpu = nullptr;
}

static double BuildAccelerationStructuresForScene(GPUScene* gpu, Span<GSInstance> instances,
                                                   Vector<GSMesh>& meshes, Vector<uint32_t>& blases,
                                                   Vector<GSCurveSet>& curves, Vector<uint32_t>& curveBlases,
                                                   Span<GSLight> lights,
                                                   ImmediateSubmitDesc const& firstSubmitDesc = {})
{
    CHECK(gpu);

    auto const buildStart = std::chrono::steady_clock::now();
    ImmediateContext ctx(RHIDeviceQueueType::Compute, GContext->device.Get());
    blases.resize(meshes.size());
    curveBlases.resize(curves.size());
    // Drivers can parallelize multiple AS builds in one command buffer, but keep each submit bounded to avoid TDR.
    constexpr size_t kBLASBuildBatch = 32u;
    bool usedFirstSubmitDesc = false;
    for (size_t i = 0; i < meshes.size(); i += kBLASBuildBatch)
    {
        size_t batchSize = std::min(kBLASBuildBatch, meshes.size() - i);
        Span<GSMesh> meshesBatch = meshes;
        Span<uint32_t> indicesBatch = blases;
        meshesBatch = meshesBatch.subspan(i, batchSize);
        indicesBatch = indicesBatch.subspan(i, batchSize);
        LOG(Editor, LogDebug, "Building BLAS {} to {}", i, i + batchSize);
        if (!usedFirstSubmitDesc)
        {
            gpu->BuildBLAS(&ctx, meshesBatch, indicesBatch, firstSubmitDesc);
            usedFirstSubmitDesc = true;
        }
        else
        {
            gpu->BuildBLAS(&ctx, meshesBatch, indicesBatch);
        }
    }
    for (size_t i = 0; i < curves.size(); i += kBLASBuildBatch)
    {
        size_t batchSize = std::min(kBLASBuildBatch, curves.size() - i);
        Span<GSCurveSet> curvesBatch = curves;
        Span<uint32_t> indicesBatch = curveBlases;
        curvesBatch = curvesBatch.subspan(i, batchSize);
        indicesBatch = indicesBatch.subspan(i, batchSize);
        LOG(Editor, LogDebug, "Building curve BLAS {} to {}", i, i + batchSize);
        if (!usedFirstSubmitDesc)
        {
            gpu->BuildCurveBLAS(&ctx, curvesBatch, indicesBatch, firstSubmitDesc);
            usedFirstSubmitDesc = true;
        }
        else
        {
            gpu->BuildCurveBLAS(&ctx, curvesBatch, indicesBatch);
        }
    }
    LOG(Editor, LogDebug, "Rebuilding TLAS");
    auto* cmd = ctx.Get();
    cmd->Begin();
    auto tlasResult = gpu->BuildTLAS(cmd, instances, blases, curveBlases, lights, false);
    cmd->End();
    if (tlasResult == GPUScene::TLASBuildResult::Built)
        ctx.Submit(), ctx.WaitIdle();
    double const buildMs = MillisecondsSince(buildStart);
    LOG(Editor, LogInfo, "Scene acceleration structures built in {:.2f} ms ({} mesh BLAS, {} curve BLAS)",
        buildMs, meshes.size(), curves.size());
    return buildMs;
}

static String PrepareScenePayloadFile(StringView path)
{
    auto ext = std::filesystem::path(path.data()).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".fscn")
        return String(path);

    // GLTF import would commit to a file first too.
    // In this way we don't need to have the whole scene allocated in memory
    // explicitly - and works the same way as loading from FSCN.
    String scenePayloadPath = PathsResolve(kTempScenePath);
    std::filesystem::path scenePayloadDir = std::filesystem::path(scenePayloadPath).parent_path();
    if (!scenePayloadDir.empty())
        std::filesystem::create_directories(scenePayloadDir);
    Allocator* importScratch = GLOBAL_ALLOC;
    MemoryMappedFile sceneFile(scenePayloadPath, 64ull * 1024ull * 1024ull /* grows on demand */);
    FImportedScene writeScene(sceneFile, importScratch);
    LoadScene(path, writeScene, importScratch);
    return scenePayloadPath;
}

struct SceneLoadStats
{
    size_t sceneMeshCount = 0;
    size_t sceneInstanceCount = 0;
    size_t sceneCurveCount = 0;
    size_t sceneMaterialCount = 0;
    size_t sceneGpuBudget = 0;
    double bufferUploadMs = 0.0;
    double textureUploadMs = 0.0;
    double blasMs = 0.0;
};

struct TextureSubresourceUploadJob
{
    uint32_t textureIndex = 0;
    uint32_t layer = 0;
    uint32_t mip = 0;
};

static void InitializeSceneLoad(FImportedScene& scene, SceneLoadStats& stats, GPUScene*& newGPUScene)
{
    auto const& sceneGlobals = scene.GetSceneGlobals();
    GEditor.shaderGlobals.camEV = sceneGlobals.postExposure;
    GEditor.viewLUTSdrIndex = static_cast<int>(sceneGlobals.viewLutSdrIndex);
    GEditor.viewLUTHdrIndex = static_cast<int>(sceneGlobals.viewLutHdrIndex);
    GEditor.viewLUTSdrExternalPath.clear();
    GEditor.viewLUTHdrExternalPath.clear();
    auto const& environment = scene.GetSceneGlobals();
    GEditor.shaderGlobals.ambientColor = environment.color;
    GEditor.shaderGlobals.ambientPower = environment.strength;
    GEditor.shaderGlobals.envAzimuthOffset = environment.azimuthOffset;
    GEditor.shaderGlobals.useEnvMap = environment.type == FSceneEnvironmentType::EnvMap &&
        environment.environmentTexture != kInvalidTexture ? 1u : 0u;
    stats.sceneMeshCount = scene.GetMeshes().size();
    stats.sceneInstanceCount = scene.GetInstances().size();
    stats.sceneCurveCount = scene.GetCurves().size();
    stats.sceneMaterialCount = scene.GetMaterials().size();
    newGPUScene = CreateGPUScene(scene, stats.sceneGpuBudget);
}

static void CompleteSceneUploadCounter(std::atomic<size_t>* counter)
{
    if (!counter)
        return;
    size_t const previous = counter->fetch_sub(1, std::memory_order_release);
    CHECK_MSG(previous > 0, "Scene upload pending job counter underflow");
    if (previous == 1)
        counter->notify_all();
}

static void WaitForPendingSceneUploadJobs(std::atomic<size_t>* counter)
{
    size_t pending = counter->load(std::memory_order_acquire);
    while (pending != 0)
    {
        counter->wait(pending, std::memory_order_relaxed);
        pending = counter->load(std::memory_order_acquire);
    }
}

template<typename TGetFootprint>
static Set<Pair<size_t, size_t>> EnqueueSceneUpload(size_t resourceCount, TGetFootprint&& getFootprint,
                                                    Allocator* alloc)
{
    Set<Pair<size_t, size_t>> pendingResources(alloc); // (footprint, source index)
    for (size_t i = 0; i < resourceCount; ++i)
    {
        size_t const footprint = getFootprint(i);
        if (footprint)
            pendingResources.insert({footprint, i});
    }
    return pendingResources;
}

static Set<Pair<size_t, size_t>> EnqueueTextureSubresourceUpload(
    FImportedScene const& scene, Vector<TextureSubresourceUploadJob>& jobs, Vector<uint32_t>& textureIDMap,
    Allocator* alloc)
{
    Set<Pair<size_t, size_t>> pendingResources(alloc); // (footprint, job index)
    for (size_t textureIndex = 0; textureIndex < scene.GetTextures().size(); ++textureIndex)
    {
        FSerializedTexture const& srcDesc = scene.GetTextures()[textureIndex];
        if (!srcDesc.IsValid() || IsSceneEnvironmentTexture(scene, textureIndex))
        {
            textureIDMap[textureIndex] = UINT32_MAX;
            continue;
        }

        CHECK_MSG(textureIndex <= std::numeric_limits<uint32_t>::max(),
                  "Too many scene textures for subresource upload job");
        uint32_t const textureIndex32 = static_cast<uint32_t>(textureIndex);
        for (uint32_t mip = 0; mip < srcDesc.GetNumMips(); ++mip)
        {
            size_t const footprint = TextureSubresourceUploadStagingFootprint(srcDesc, 0u, mip);
            if (!footprint)
                continue;

            for (uint32_t layer = 0; layer < srcDesc.GetNumLayers(); ++layer)
            {
                size_t const idx = jobs.size();
                jobs.push_back({
                    .textureIndex = textureIndex32,
                    .layer = layer,
                    .mip = mip,
                });
                pendingResources.insert({footprint, idx});
            }
        }
    }
    return pendingResources;
}

template<typename TTryUpload, typename TFlushUpload, typename TGetRemainingStaging>
static void ScheduleSceneUpload(Set<Pair<size_t, size_t>>& pendingResources, const char* resourceLabel,
                                TTryUpload&& tryUpload, TFlushUpload&& flushUpload,
                                TGetRemainingStaging&& getRemainingStaging)
{
    while (!pendingResources.empty())
    {
        size_t remainingStaging = getRemainingStaging();
        // Take largest job we can fit (best-fit) to staging
        auto it = pendingResources.upper_bound({remainingStaging, std::numeric_limits<size_t>::max()});
        if (it == pendingResources.begin())
        {
            LOG(Editor, LogDebug, "Scene upload staging exhausted before {} upload ({} bytes remaining); flushing",
                resourceLabel, remainingStaging);
            flushUpload();
            remainingStaging = getRemainingStaging();
            it = pendingResources.upper_bound({remainingStaging, std::numeric_limits<size_t>::max()});
            CHECK_MSG(it != pendingResources.begin(),
                      "Staging buffer too small for any pending {} upload ({} bytes available, smallest pending {} bytes)",
                      resourceLabel, remainingStaging, pendingResources.begin()->first);
        }

        --it;
        Pair<size_t, size_t> const job = *it;
        pendingResources.erase(it);

        if (!tryUpload(job.second, job.first))
        {
            LOG(Editor, LogDebug, "Scene upload staging exhausted by {} {} upload ({} bytes needed); flushing",
                resourceLabel, job.second, job.first);
            flushUpload();
            CHECK_MSG(tryUpload(job.second, job.first),
                      "Staging buffer too small for single {} upload ({} {}, {} bytes)",
                      resourceLabel, resourceLabel, job.second, job.first);
        }
    }
}

struct SceneUploadJob : ThreadPoolJob
{
    GPUScene::StagedUploadJob job;
    Span<Arena> scratchArenas;
    Span<AllocatorStack> scratchAllocators;
    std::atomic<size_t>* batchCounter;
    std::atomic<size_t>* dependencyCounter;
    std::atomic<double>* uploadEndMs;
    std::chrono::steady_clock::time_point uploadStatStart;

    SceneUploadJob(GPUScene::StagedUploadJob const& job, Span<Arena> scratchArenas,
                   Span<AllocatorStack> scratchAllocators, std::atomic<size_t>* batchCounter,
                   std::atomic<size_t>* dependencyCounter, std::atomic<double>* uploadEndMs,
                   std::chrono::steady_clock::time_point uploadStatStart) :
        job(job), scratchArenas(scratchArenas), scratchAllocators(scratchAllocators),
        batchCounter(batchCounter), dependencyCounter(dependencyCounter), uploadEndMs(uploadEndMs),
        uploadStatStart(uploadStatStart)
    {
    }

    void Execute(size_t workerID) noexcept override
    {
        if (job.NeedsScratch())
        {
            AllocatorStack& scratch = scratchAllocators[workerID];
            scratch.Reset(scratchArenas[workerID]);
            job.Write(&scratch);
        }
        else
        {
            job.Write();
        }

        if (uploadEndMs)
            InterlockedMax(*uploadEndMs, MillisecondsSince(uploadStatStart), std::memory_order_release);

        CompleteSceneUploadCounter(batchCounter);
        if (dependencyCounter != batchCounter)
            CompleteSceneUploadCounter(dependencyCounter);
    }
};

static void UploadLoadedSceneToGPU(FImportedScene& scene, GPUScene* gpu, SceneLoadStats& stats,
                                   AllocatorStack& sceneAlloc)
{
    Vector<uint32_t> meshOffsets(scene.GetMeshes().size(), &sceneAlloc);
    Vector<uint32_t> curveOffsets(scene.GetCurves().size(), &sceneAlloc);
    Vector<uint32_t> textureIDMap(scene.GetTextures().size(), UINT32_MAX, &sceneAlloc);
    auto const& environment = scene.GetSceneGlobals();
    bool const hasEnvironmentTexture = environment.type == FSceneEnvironmentType::EnvMap &&
        environment.environmentTexture != kInvalidTexture;

    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    size_t blobBudget = 0;
    size_t blobLaneBudget = 0;
    size_t blobWorkerCount = 0;
    auto const uploadStart = std::chrono::steady_clock::now();
    {
        std::atomic<double> bufferUploadEndMs{0};
        std::atomic<double> textureUploadEndMs{0};
        std::atomic<double> blasDurationMs{0};
        size_t stagedTaskCount = 0;
        stagedTaskCount += scene.GetMeshes().size() * 7u;
        stagedTaskCount += scene.GetCurves().size() * 4u;
        size_t textureSubresourceCount = 0;
        for (size_t textureIndex = 0; textureIndex < scene.GetTextures().size(); ++textureIndex)
        {
            FSerializedTexture const& srcDesc = scene.GetTextures()[textureIndex];
            if (srcDesc.IsValid() && !IsSceneEnvironmentTexture(scene, textureIndex))
                textureSubresourceCount += size_t(srcDesc.GetNumLayers()) * srcDesc.GetNumMips();
        }
        stagedTaskCount += textureSubresourceCount;

        // Scratch memory for blob decode. Each upload worker gets one lane, large enough for the largest decoded blob.
        const bool directGeometryUpload = gpu->UsesDirectGeometryUpload();
        const size_t geometryResourceCount = stats.sceneMeshCount + stats.sceneCurveCount;
        constexpr size_t kUploadBlockingTaskCount = 2u; // BLAS build + final texture wait
        const size_t hardwareThreadCount = std::max<size_t>(1u, std::thread::hardware_concurrency());
        blobWorkerCount = std::min(hardwareThreadCount,
                                   std::max<size_t>(1u, stagedTaskCount + kUploadBlockingTaskCount));
        blobLaneBudget = SceneBlobScratchBudget(scene);
        blobBudget = blobLaneBudget * blobWorkerCount;
        ScopedArena blobArena(GLOBAL_ALLOC, blobBudget);
        CHECK(blobArena);
        Span<Arena> blobScratchArenas = ConstructSpan<Arena>(&sceneAlloc, blobWorkerCount);
        Span<AllocatorStack> blobScratchAllocators = ConstructSpan<AllocatorStack>(&sceneAlloc, blobWorkerCount);
        char* blobMemory = static_cast<char*>(blobArena.arena.memory);
        for (size_t i = 0; i < blobWorkerCount; ++i)
        {
            Arena workerArena{blobMemory + i * blobLaneBudget, blobLaneBudget};
            blobScratchArenas[i] = workerArena;
            blobScratchAllocators[i].Reset(workerArena);
        }

        ThreadPool uploadPool(blobWorkerCount, GetSceneUploadTaskQueueSize(stagedTaskCount + kUploadBlockingTaskCount),
                              &sceneAlloc, "SceneUpload");
        Vector<GPUScene::StagedUploadJob> stagedJobs(&sceneAlloc);
        std::atomic<size_t> uploadBatchStagedJobs{0};
        std::atomic<size_t> geometryStagedJobs{0};
        auto ScheduleStagedJobs = [&](std::atomic<size_t>* dependencyCounter,
                                      std::atomic<double>* uploadEndMs,
                                      std::chrono::steady_clock::time_point uploadStatStart)
        {
            size_t const jobCount = stagedJobs.size();
            if (jobCount == 0)
                return;
            uploadBatchStagedJobs.fetch_add(jobCount, std::memory_order_relaxed);
            if (dependencyCounter)
                dependencyCounter->fetch_add(jobCount, std::memory_order_relaxed);
            for (GPUScene::StagedUploadJob const& job : stagedJobs)
                uploadPool.PushImpl<SceneUploadJob>(job, blobScratchArenas, blobScratchAllocators,
                                                    &uploadBatchStagedJobs, dependencyCounter,
                                                    uploadEndMs, uploadStatStart);
            stagedJobs.clear();
        };
        auto WaitForUploadBatchStagedJobs = [&]
        {
            WaitForPendingSceneUploadJobs(&uploadBatchStagedJobs);
        };
        auto WaitForGeometryStagedJobs = [&]
        {
            WaitForPendingSceneUploadJobs(&geometryStagedJobs);
        };
        auto FenceStagedJobs = [&]
        {
            uploadPool.Join();
            for (size_t i = 0; i < blobScratchAllocators.size(); ++i)
                blobScratchAllocators[i].Reset(blobScratchArenas[i]);
        };
        constexpr size_t kGeometryUploadReadyValue = 1u;
        constexpr size_t kTextureUploadReadyValue = 2u;
        constexpr size_t kSceneUploadStagingBuffers = 3u;
        auto uploadTimeline = GContext->device->CreateSemaphore(true);
        const size_t uploadStaging = SceneStagingBufferBudget(scene, directGeometryUpload);
        LOG(Editor, LogInfo, "Scene CPU Staging Budget: {} bytes per lane ({} lanes, {} bytes total)",
            uploadStaging, kSceneUploadStagingBuffers, uploadStaging * kSceneUploadStagingBuffers);
        ImmediateUpload upload(GContext->device.Get(), uploadStaging, RHIDeviceQueueType::Transfer,
                               kSceneUploadStagingBuffers);
        upload.Begin();
        auto FlushUpload = [&]
        {
            WaitForUploadBatchStagedJobs();
            upload.End();
            upload.Begin();
        };

        // Geometry Upload
        // See also SceneStagingBufferBudget
        GEditor.meshes.resize(scene.GetMeshes().size());
        auto pendingMeshes = EnqueueSceneUpload(
            scene.GetMeshes().size(),
            [&](size_t meshIndex)
            {
                return directGeometryUpload
                    ? size_t(1)
                    : GPUScene::CalculateMeshPrimitiveSize(scene.GetMeshes()[meshIndex]);
            },
            &sceneAlloc);
        ScheduleSceneUpload(
            pendingMeshes, "mesh",
            [&](size_t meshIndex, size_t)
            {
                auto const& srcDesc = scene.GetMeshes()[meshIndex];
                auto& dst = GEditor.meshes[meshIndex];
                auto& offset = meshOffsets[meshIndex];
                stagedJobs.clear();
                bool const uploaded = gpu->BeginUpload(&upload, scene, srcDesc, dst, offset, stagedJobs) != 0;
                if (uploaded)
                    ScheduleStagedJobs(&geometryStagedJobs, &bufferUploadEndMs, uploadStart);
                else
                    stagedJobs.clear();
                return uploaded;
            },
            FlushUpload,
            [&]
            {
                return static_cast<size_t>(upload.end - upload.ptr);
            });

        GEditor.curves.resize(scene.GetCurves().size());
        auto pendingCurves = EnqueueSceneUpload(
            scene.GetCurves().size(),
            [&](size_t curveIndex)
            {
                if (directGeometryUpload)
                    return size_t(1);

                auto const& srcDesc = scene.GetCurves()[curveIndex];
                return GPUScene::CalculateCurvePrimitiveSize(srcDesc) + GPUScene::CalculateCurveAABBSize(srcDesc);
            },
            &sceneAlloc);
        ScheduleSceneUpload(
            pendingCurves, "curve",
            [&](size_t curveIndex, size_t)
            {
                auto const& srcDesc = scene.GetCurves()[curveIndex];
                auto& dst = GEditor.curves[curveIndex];
                auto& offset = curveOffsets[curveIndex];
                stagedJobs.clear();
                bool const uploaded = gpu->BeginUpload(&upload, scene, srcDesc, dst, offset, stagedJobs) != 0;
                if (uploaded)
                    ScheduleStagedJobs(&geometryStagedJobs, &bufferUploadEndMs, uploadStart);
                else
                    stagedJobs.clear();
                return uploaded;
            },
            FlushUpload,
            [&]
            {
                return static_cast<size_t>(upload.end - upload.ptr);
            });
        WaitForGeometryStagedJobs();
        if (gpu->UsesDirectGeometryUpload())
            gpu->FlushDirectGeometryUpload();
        RHIDeviceQueue::TimelinePair geometryUploadSignal{uploadTimeline.Get(), kGeometryUploadReadyValue};
        upload.End(ImmediateSubmitDesc{.timelineSignals = {&geometryUploadSignal, 1}});

        // Once we have geometry (waited with timeline) and instances, overlap BLAS build on another thread (job).
        BuildEditorInstances(scene, GEditor.instances, meshOffsets, curveOffsets);
        BuildSceneLights(scene, GEditor.lights, GEditor.shaderGlobals, gpu->mLightSamplerType);
        uploadPool.Push([&]
        {
            RHIDeviceQueue::TimelinePair wait{uploadTimeline.Get(), kGeometryUploadReadyValue};
            RHIPipelineStage waitStage = RHIPipelineStageBits::AccelerationBuild;
            double const buildMs = BuildAccelerationStructuresForScene(
                gpu, GEditor.instances, GEditor.meshes, GEditor.blases, GEditor.curves, GEditor.curveBlases, GEditor.lights,
                ImmediateSubmitDesc{.timelineWaits = {&wait, 1}, .waitStages = {&waitStage, 1}});
            blasDurationMs.store(buildMs, std::memory_order_release);
        });
        if (geometryResourceCount != 0)
            InterlockedMax(bufferUploadEndMs, MillisecondsSince(uploadStart), std::memory_order_release);
        upload.Begin();
        stats.bufferUploadMs = bufferUploadEndMs;
        auto const textureUploadStart = std::chrono::steady_clock::now();
        bool anyTextureUpload = false;

        // Texture Upload
        // Texture payloads use the same blob route as geometry; compressed blobs decode through worker scratch.
        Vector<TextureSubresourceUploadJob> textureJobs(&sceneAlloc);
        textureJobs.reserve(textureSubresourceCount);
        Vector<Optional<GPUScene::TextureUpload>> textureUploads(scene.GetTextures().size(), &sceneAlloc);
        auto pendingTextures = EnqueueTextureSubresourceUpload(scene, textureJobs, textureIDMap, &sceneAlloc);
        ScheduleSceneUpload(
            pendingTextures, "texture subresource",
            [&](size_t idx, size_t)
            {
                TextureSubresourceUploadJob const& job = textureJobs[idx];
                size_t const textureIndex = job.textureIndex;
                FSerializedTexture const& srcDesc = scene.GetTextures()[textureIndex];
                auto& textureUpload = textureUploads[textureIndex];
                if (!textureUpload)
                    textureUpload.emplace(gpu->BeginTextureUpload(&upload, srcDesc));
                stagedJobs.clear();
                bool const uploaded = gpu->BeginTextureSubresourceUpload(
                    &upload, scene, srcDesc, *textureUpload, job.layer, job.mip, stagedJobs) != 0;
                if (uploaded)
                {
                    anyTextureUpload = true;
                    ScheduleStagedJobs(nullptr, &textureUploadEndMs, textureUploadStart);
                }
                else
                {
                    stagedJobs.clear();
                }
                return uploaded;
            },
            FlushUpload,
            [&]
            {
                return static_cast<size_t>(upload.end - upload.ptr);
            });
        for (size_t textureIndex = 0; textureIndex < textureUploads.size(); ++textureIndex)
        {
            if (textureUploads[textureIndex])
                gpu->EndTextureUpload(&upload, std::move(*textureUploads[textureIndex]), textureIDMap[textureIndex]);
        }
        if (anyTextureUpload)
        {
            uploadPool.Push([&]
            {
                WaitForPendingSceneUploadJobs(&uploadBatchStagedJobs);

                RHIDeviceQueue::TimelinePair signal{uploadTimeline.Get(), kTextureUploadReadyValue};
                upload.End(ImmediateSubmitDesc{.timelineSignals = {&signal, 1}});
                GContext->device->WaitForTimelineSemaphores(Span<const RHIDeviceQueue::TimelinePair>(&signal, 1), -1);
                InterlockedMax(textureUploadEndMs, MillisecondsSince(textureUploadStart), std::memory_order_release);
            });
        }
        else
        {
            upload.End(), upload.WaitIdle();
        }

        FenceStagedJobs();
        stats.blasMs = blasDurationMs;
        stats.textureUploadMs = textureUploadEndMs;
        FTexture sdr(&sceneAlloc);
        FTexture hdr(&sceneAlloc);
        upload.Begin();
        if (hasEnvironmentTexture)
        {
            CHECK_MSG(environment.environmentTexture < scene.GetTextures().size(),
                      "Scene environment texture index out of range");
            FSerializedTexture const& environmentTextureDesc = scene.GetTextures()[environment.environmentTexture];
            ScopedArena environmentArena(GLOBAL_ALLOC, SceneTextureReadBudget(environmentTextureDesc));
            CHECK(environmentArena);
            AllocatorStack environmentAlloc(environmentArena);
            FTexture environmentTexture = ReadSceneTexture(scene, environmentTextureDesc, &environmentAlloc);
            CHECK_MSG(environmentTexture.GetFormat() == RHIResourceFormat::R32G32B32A32SignedFloat,
                      "Scene environment texture must be RGBA32F, got {}", environmentTexture.GetFormat());
            gpu->UploadEnvMap(&upload, environmentTexture);
        }
        LoadSelectedViewLUTs(sdr, hdr, GEditor.viewLUTSdrIndex, GEditor.viewLUTHdrIndex,
                             GEditor.viewLUTSdrExternalPath, GEditor.viewLUTHdrExternalPath, &sceneAlloc);
        gpu->UploadViewLUTs(&upload, sdr, hdr);
        upload.End(), upload.WaitIdle();
    }
    double uploadWallMs = MillisecondsSince(uploadStart);
    LOG(Editor, LogInfo, "Scene GPU upload complete (wall {:.2f} ms, buffer upload {:.2f} ms, texture upload {:.2f} ms, TX {:.2f} MB/s, BLAS {:.2f} ms, wall-sum {:.2f} ms, scratch per lane {:.2f} MB, total {:.2f} MB across {} workers)",
        uploadWallMs, stats.bufferUploadMs, stats.textureUploadMs,
        stats.sceneGpuBudget / (1 << 20) / ((stats.bufferUploadMs + stats.textureUploadMs)  / 1000.0),
        stats.blasMs,
        uploadWallMs - (stats.bufferUploadMs + stats.textureUploadMs + stats.blasMs),
        double(blobLaneBudget) / double(1u << 20), double(blobBudget) / double(1u << 20), blobWorkerCount);

    BuildEditorMaterials(scene, GEditor.materials, textureIDMap);
    ApplySceneCamera(scene, GEditor.camera, GEditor.aperture, GEditor.shaderGlobals);
    GEditor.shaderGlobals.ggxLutEIndex = gpu->GetGGXLutEIndex();
    GEditor.shaderGlobals.sheenLtcIndex = gpu->GetSheenLtcIndex();
    GEditor.shaderGlobals.viewLutIndex = GContext->enableHDR
        ? gpu->GetViewLutHdrIndex()
        : gpu->GetViewLutSdrIndex();
    GEditor.shaderGlobals.envMapTextureIndex = gpu->GetEnvMapIndexOrDefault();
    GEditor.shaderGlobals.envMapMarginalCDFIndex = gpu->GetEnvMapMarginalCDFIndexOrDefault();
    GEditor.shaderGlobals.envMapConditionalCDFIndex = gpu->GetEnvMapConditionalCDFIndexOrDefault();
    CommitSceneToGPU(gpu, GEditor.instances, GEditor.materials, GEditor.lights, GEditor.shaderGlobals, true);
}

static void InstallLoadedScene(String const& scenePayloadPath, GPUScene*& newGPUScene)
{
    GContext->device->WaitIdle();
    DestroyEditorRenderer(GContext);
    DestroyGPUScene(GContext->gpuScene);
    GContext->gpuScene = newGPUScene;
    newGPUScene = nullptr;
    GEditor.OpenSceneFile(scenePayloadPath);
    GEditor.selectedInstance = -1;
    GEditor.selectedMaterial = -1;
    GEditor.selectedLight = -1;
    GEditor.cameraUpdated = true;
    GEditor.state = FERunningEnter;
}

void LoadScene(StringView path)
{
    LOG(Editor, LogInfo, "Loading scene: {}", path);

    auto const loadStart = std::chrono::steady_clock::now();

    GPUScene* gpu = nullptr;
    try
    {
        String scenePayloadPath = PrepareScenePayloadFile(path);
        // Scratch memory for scene loading.
        // Do note that from here on out, it's all FSCN loading, where
        // the few places that intermediate allocation is required are:
        // - Offset maps
        // - Various small allocations
        // - Working allocator for ThreadPool
        ScopedArena sceneArena(GLOBAL_ALLOC, kDefaultSceneLoadScratchBudget);
        AllocatorStack sceneAlloc(sceneArena);
        MemoryMappedFile loadSceneFile(scenePayloadPath, MemoryMappedAccess::ReadOnly);
        FImportedScene scene(loadSceneFile, &sceneAlloc);
        LoadFSCN(scene);

        SceneLoadStats loadedScene;
        InitializeSceneLoad(scene, loadedScene, gpu);
        UploadLoadedSceneToGPU(scene, gpu, loadedScene, sceneAlloc);
        InstallLoadedScene(scenePayloadPath, gpu);

        size_t currentLoadScratchUsed = 0;
        size_t loadScratchBudget = 0;
        sceneAlloc.QueryBudget(currentLoadScratchUsed, loadScratchBudget);
        double const loadMs = MillisecondsSince(loadStart);
        LOG(Editor, LogInfo, "Scene load complete in {:.2f} ms, {} meshes, {} instances, {} curves, {} materials",
            loadMs, loadedScene.sceneMeshCount,
            loadedScene.sceneInstanceCount, loadedScene.sceneCurveCount, loadedScene.sceneMaterialCount);
    }
    catch (std::exception const& e)
    {
        DestroyGPUScene(gpu);
        LOG(Editor, LogError, "Failed to load scene: {} ({})", path, e.what());
    }
    catch (...)
    {
        DestroyGPUScene(gpu);
        LOG(Editor, LogError, "Failed to load scene: {}", path);
    }
}

void LoadEnvMap(StringView path)
{
    LOG(Editor, LogInfo, "Loading HDRI env map: {}", path);
    auto* gpu = GContext->gpuScene;
    if (!gpu)
    {
        LOG(Editor, LogWarn, "Ignoring HDRI env map load before a scene is loaded");
        return;
    }
    try
    {
        FTexture tex(GLOBAL_ALLOC);
        LoadHDR(tex, path);
        ImmediateUpload upload(GContext->device.Get(), EnvMapUploadStagingBudget(tex));
        upload.Begin();
        gpu->UploadEnvMap(&upload, tex);
        upload.End(), upload.WaitIdle();
        GEditor.Scene().GetSceneGlobals() = {
            .type = FSceneEnvironmentType::EnvMap,
            .color = GEditor.shaderGlobals.ambientColor,
            .strength = GEditor.shaderGlobals.ambientPower,
            .azimuthOffset = GEditor.shaderGlobals.envAzimuthOffset,
        };
        GEditor.shaderGlobals.useEnvMap = 1u;
        GEditor.shaderGlobals.envMapTextureIndex = gpu->GetEnvMapIndexOrDefault();
        GEditor.shaderGlobals.envMapMarginalCDFIndex = gpu->GetEnvMapMarginalCDFIndexOrDefault();
        GEditor.shaderGlobals.envMapConditionalCDFIndex = gpu->GetEnvMapConditionalCDFIndexOrDefault();
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
        LOG(Editor, LogInfo, "HDRI env map loaded successfully");
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to load HDRI env map: {}", path);
    }
}

void HandleFile(const char* filePath)
{
    auto path = std::filesystem::path(filePath);
    auto ext = path.extension().string();
    // Normalize to lowercase
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".gltf" || ext == ".glb" || ext == ".fscn")
    {
        LoadScene(filePath);
        // And, with HDRIs sharing the same filename (if any), load them up too
        String hdriPath = path.string().substr(0, path.string().length() - ext.length());
        if (std::filesystem::exists(hdriPath + ".hdr"))
            LoadEnvMap(hdriPath + ".hdr");
        else if (std::filesystem::exists(hdriPath + ".hdri"))
            LoadEnvMap(hdriPath + ".hdri");
    }
    else if (ext == ".hdr" || ext == ".hdri")
    {
        LoadEnvMap(filePath);
    }
    else
    {
        LOG(Editor, LogWarn, "Unknown file type dropped: '{}'", filePath);
    }
}