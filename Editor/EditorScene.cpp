#include <Core/Paths.hpp>
#include <filesystem>
#include <limits>
#include <numbers>

#include "EditorState.hpp"
#include "Scene/Mesh.hpp"
#include "Renderer/GPUScene.hpp"
static constexpr const char* kTempScenePath = "Cache/Last.fscn";
static constexpr size_t kDefaultSceneLoadScratchBudget = 64ull * (1ull << 20);
// Accounts for alignment, etc...
static constexpr size_t kBudgetSlack = 1ull * (1ull << 20);
static constexpr size_t kStagingBudgetSlack = 32ull * (1ull << 20);

// Fills the GPUScene-owned instance/material/light table spans from the editor scene
// (geometry handles + texture remap live in EditorState), then commits them.
static GPUScene::UpdateResult BuildSceneTables(GPUScene* gpu, FImportedScene& scene);

static GPUScene::UpdateResult CommitSceneToGPU(GPUScene* gpu, FImportedScene& scene, UBO& globals,
                                               bool resetAccumulation)
{
    CHECK(gpu);

    auto res = BuildSceneTables(gpu, scene);
    globals.firstInstance = res.firstInstance;
    globals.numInstances  = res.numInstances;
    globals.firstMaterial = res.firstMaterial;
    globals.numMaterials  = res.numMaterials;
    globals.firstLight    = res.firstLight;
    globals.numSceneLights = res.numLights;
    globals.firstLightAliasTable = res.firstLightAliasTable;
    globals.sceneLightWeightSum = res.sceneLightWeightSum;
    if (resetAccumulation)
        globals.ptAccumulatedFrames = 0;
    return res;
}

void CommitSceneToGPU(bool resetAccumulation)
{
    if (!GContext->gpuScene || !GEditor.HasScene())
        return;

    CommitSceneToGPU(GContext->gpuScene, GEditor.Scene(), GEditor.shaderGlobals, resetAccumulation);
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

static bool IsSceneEnvironmentTexture(FImportedScene const& scene, size_t textureIndex)
{
    auto const& environment = scene.GetSceneGlobals();
    return environment.type == FSceneEnvironmentType::EnvMap &&
        environment.environmentTexture != kInvalidTexture &&
        textureIndex == environment.environmentTexture;
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
        GContext->gpuScene->UploadViewLUTs(sdr, hdr);
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

    GContext->gpuScene->FillGlobals(GEditor.shaderGlobals, GContext->enableHDR);
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

static void FillGSMaterial(GSMaterial& dst, FMaterial const& src, Vector<TextureHandle> const& textureIDMap)
{
    auto RemapTextureIndex = [&textureIDMap](uint32_t index) -> uint32_t {
        if (index == UINT32_MAX || index >= textureIDMap.size())
            return UINT32_MAX;
        return textureIDMap[index].index;
    };
    dst.baseColorFactor = src.baseColorFactor;
    // emissiveFactor.w is the editor's emissive intensity; bake it into the GPU RGB.
    dst.emissiveFactor = float3(src.emissiveFactor) * src.emissiveFactor.w;
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

// Fills the GPUScene-owned mapped table spans directly from the editor scene. Scene
// semantics (geometry handle binding, material/texture remap, light conversion) stay
// in the editor; GPUScene only owns the mapped ring memory and commit.
static GPUScene::UpdateResult BuildSceneTables(GPUScene* gpu, FImportedScene& scene)
{
    auto instances = scene.GetInstances();
    auto materials = scene.GetMaterials();
    auto lights = scene.GetLights();
    CHECK_MSG(instances.size() <= UINT32_MAX && materials.size() <= UINT32_MAX && lights.size() <= UINT32_MAX,
              "Scene table exceeds uint32_t range");
    auto tables = gpu->BeginScene(static_cast<uint32_t>(instances.size()),
                                  static_cast<uint32_t>(materials.size()),
                                  static_cast<uint32_t>(lights.size()));
    for (size_t i = 0; i < instances.size(); ++i)
    {
        auto const& src = instances[i];
        GeometryHandle geometry;
        if (src.type == FInstanceType::Mesh)
        {
            CHECK_MSG(src.resourceIndex < GEditor.meshGeometry.size(),
                      "Mesh instance references invalid mesh {}", src.resourceIndex);
            geometry = GEditor.meshGeometry[src.resourceIndex];
        }
        else if (src.type == FInstanceType::Curve)
        {
            CHECK_MSG(src.resourceIndex < GEditor.curveGeometry.size(),
                      "Curve instance references invalid curve {}", src.resourceIndex);
            geometry = GEditor.curveGeometry[src.resourceIndex];
        }
        else
            CHECK_MSG(false, "Unknown scene instance type {}", static_cast<uint32_t>(src.type));
        tables.instances[i] = InstanceDesc{
            .geometry = geometry,
            .transform = src.transform.transform,
            .rotation = src.transform.rotation,
            .scale = src.transform.scale,
            .materialIndex = src.materialIndex,
        };
    }
    for (size_t i = 0; i < materials.size(); ++i)
        FillGSMaterial(tables.materials[i], materials[i], GEditor.textureIDMap);
    for (size_t i = 0; i < lights.size(); ++i)
        FLightToGSLight(lights[i], tables.lights[i], gpu->mLightSamplerType);
    return gpu->EndScene(tables);
}

void UpdateSceneLights()
{
    if (!GEditor.HasScene() || !GContext->gpuScene)
        return;
    CommitSceneToGPU(false);
}

void DeleteSelectedInstance()
{
    if (!GContext->gpuScene || !GEditor.HasScene())
        return;
    int const idx = GEditor.selectedInstance;
    auto& sceneInstances = GEditor.Scene().mTables.instances;
    if (idx < 0 || idx >= static_cast<int>(sceneInstances.size()))
        return;

    sceneInstances.erase(sceneInstances.begin() + idx);
    GEditor.selectedInstance = -1;
    GEditor.selectedMaterial = -1;
    // Recommit the (now smaller) instance table, then reclaim geometry no longer
    // referenced. Wait for the GPU to finish before reclaiming resident resources.
    CommitSceneToGPU(true);
    GContext->device->WaitIdle();
    GContext->gpuScene->Collect();
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

static size_t GPUSceneBudgetBytes(GPUSceneDesc const& desc)
{
    return size_t(desc.primitiveBudget) +
           size_t(desc.curveAABBBudget) +
           size_t(desc.instanceBudget) * sizeof(GSInstance) +
           size_t(desc.materialBudget) * sizeof(GSMaterial) +
           size_t(desc.lightBudget) * sizeof(GSLight) +
           size_t(desc.lightBudget) * sizeof(GSAlias) +
           size_t(desc.tlasInstanceBudget) * GContext->device->WriteAccelerationStructureInstanceData({}, nullptr) +
           size_t(desc.tlasBudget) +
           size_t(desc.tlasScratchBudget);
}

static GPUScene* CreateGPUScene(FImportedScene const& scene, size_t& outBudgetBytes)
{
    auto estimatedBudget = scene.CalculateGPUSceneDesc(GContext->device->GetCapabilities());
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

    auto* gpu = Construct<GPUScene>(GContext->allocator, GContext->device.Get(), GContext->allocator,
                                    estimatedBudget, GContext->editorFrameScratch.get());
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
    double uploadMs = 0.0;
};

// Applies the new scene's editor globals (exposure, ambient, env, view LUT selection).
// Deferred to install time so it doesn't perturb the currently-rendering scene while a
// background load is in flight.
static void ApplySceneGlobals(FImportedScene const& scene)
{
    auto const& sceneGlobals = scene.GetSceneGlobals();
    GEditor.shaderGlobals.camEV = sceneGlobals.postExposure;
    GEditor.viewLUTSdrIndex = static_cast<int>(sceneGlobals.viewLutSdrIndex);
    GEditor.viewLUTHdrIndex = static_cast<int>(sceneGlobals.viewLutHdrIndex);
    GEditor.viewLUTSdrExternalPath.clear();
    GEditor.viewLUTHdrExternalPath.clear();
    GEditor.shaderGlobals.ambientColor = sceneGlobals.color;
    GEditor.shaderGlobals.ambientPower = sceneGlobals.strength;
    GEditor.shaderGlobals.envAzimuthOffset = sceneGlobals.azimuthOffset;
    GEditor.shaderGlobals.useEnvMap = sceneGlobals.type == FSceneEnvironmentType::EnvMap &&
        sceneGlobals.environmentTexture != kInvalidTexture ? 1u : 0u;
}

static void InitializeSceneLoad(FImportedScene& scene, SceneLoadStats& stats, GPUScene*& newGPUScene)
{
    stats.sceneMeshCount = scene.GetMeshes().size();
    stats.sceneInstanceCount = scene.GetInstances().size();
    stats.sceneCurveCount = scene.GetCurves().size();
    stats.sceneMaterialCount = scene.GetMaterials().size();
    newGPUScene = CreateGPUScene(scene, stats.sceneGpuBudget);
}

// Submits every scene geometry/texture to GPUScene's work queue. The queue drains on a
// background thread (kicked by the first Poll() in PumpSceneLoad); the GPUScene is not
// yet installed, so the worker owns it exclusively (no shared state with the rendering
// thread). Finalized by FinalizeSceneUpload once Poll() reports Ready.
static void BeginSceneUpload(FImportedScene& scene, GPUScene* gpu, Vector<GeometryHandle>& meshGeometry,
                             Vector<GeometryHandle>& curveGeometry, Vector<TextureHandle>& textureIDMap)
{
    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    FBlobDeserializer blobs = scene.GetBlobDeserializer();

    // Geometry uploads: each returns a resident handle, recorded by resource index so
    // BuildSceneTables can bind instances to geometry. These maps stay in the pending
    // load (not GEditor) until install, so edits to the still-current scene aren't
    // mismatched against the loading one.
    meshGeometry.clear();
    curveGeometry.clear();
    meshGeometry.reserve(scene.GetMeshes().size());
    curveGeometry.reserve(scene.GetCurves().size());
    for (FSerializedMesh const& mesh : scene.GetMeshes())
    {
        GeometryHandle handle;
        GPUScene::Result r = gpu->Upload(&blobs, mesh, handle);
        CHECK_MSG(r == GPUScene::Result::InProgress, "Mesh upload rejected ({})", static_cast<int>(r));
        meshGeometry.push_back(handle);
    }
    for (FSerializedCurve const& curve : scene.GetCurves())
    {
        GeometryHandle handle;
        GPUScene::Result r = gpu->Upload(&blobs, curve, handle);
        CHECK_MSG(r == GPUScene::Result::InProgress, "Curve upload rejected ({})", static_cast<int>(r));
        curveGeometry.push_back(handle);
    }

    // Texture uploads (the environment map is handled separately in finalize).
    textureIDMap.assign(scene.GetTextures().size(), TextureHandle{});
    for (size_t textureIndex = 0; textureIndex < scene.GetTextures().size(); ++textureIndex)
    {
        FSerializedTexture const& srcDesc = scene.GetTextures()[textureIndex];
        if (!srcDesc.IsValid() || IsSceneEnvironmentTexture(scene, textureIndex))
            continue;
        TextureHandle handle;
        GPUScene::Result r = gpu->Upload(&blobs, srcDesc, handle);
        CHECK_MSG(r == GPUScene::Result::InProgress, "Texture {} upload rejected ({})", textureIndex,
                  static_cast<int>(r));
        textureIDMap[textureIndex] = handle;
    }
    // The queued geometry/textures drain lazily on the first Poll() (PumpSceneLoad),
    // on a background thread, while the editor keeps rendering the current/idle scene.
}

// Runs once the background drain completes: env map / view LUTs, scene globals, table
// commit, and the initial TLAS build. All on the calling (main) thread.
static void FinalizeSceneUpload(FImportedScene& scene, GPUScene* gpu, SceneLoadStats& stats,
                                AllocatorStack& sceneAlloc)
{
    ApplySceneGlobals(scene);
    auto const& environment = scene.GetSceneGlobals();
    bool const hasEnvironmentTexture = environment.type == FSceneEnvironmentType::EnvMap &&
        environment.environmentTexture != kInvalidTexture;

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
        gpu->UploadEnvMap(environmentTexture);
    }
    {
        FTexture sdr(&sceneAlloc);
        FTexture hdr(&sceneAlloc);
        LoadSelectedViewLUTs(sdr, hdr, GEditor.viewLUTSdrIndex, GEditor.viewLUTHdrIndex,
                             GEditor.viewLUTSdrExternalPath, GEditor.viewLUTHdrExternalPath, &sceneAlloc);
        gpu->UploadViewLUTs(sdr, hdr);
    }

    LOG(Editor, LogInfo, "Scene GPU upload complete in {:.2f} ms", stats.uploadMs);

    ApplySceneCamera(scene, GEditor.camera, GEditor.aperture, GEditor.shaderGlobals);
    gpu->FillGlobals(GEditor.shaderGlobals, GContext->enableHDR);

    // Fill instance/material/light tables (geometry handles are now Ready), then TLAS.
    CommitSceneToGPU(gpu, scene, GEditor.shaderGlobals, true);
    ImmediateContext ctx(RHIDeviceQueueType::Compute, GContext->device.Get());
    auto* cmd = ctx.Get();
    cmd->Begin();
    auto tlasResult = gpu->BuildTLAS(cmd, false);
    cmd->End();
    if (tlasResult == GPUScene::TLASBuildResult::Built)
        ctx.Submit(), ctx.WaitIdle();
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

// In-flight async scene load. The FSCN scratch arena, mapped file, and parsed scene must
// outlive the background GPUScene drain (which holds pointers into the scene's serialized
// data + payload), so they live here until FinalizeSceneUpload + install completes.
struct PendingSceneLoad
{
    String scenePayloadPath;
    Optional<ScopedArena> arena;
    Optional<AllocatorStack> alloc;
    Optional<MemoryMappedFile> file;
    Optional<FImportedScene> scene;
    GPUScene* gpu{nullptr};
    SceneLoadStats stats;
    std::chrono::steady_clock::time_point loadStart;
    String envMapPath; // optional HDRI to load once the scene is installed
    // Scene->geometry/texture bindings, moved into GEditor only at install time.
    Vector<GeometryHandle> meshGeometry{GLOBAL_ALLOC};
    Vector<GeometryHandle> curveGeometry{GLOBAL_ALLOC};
    Vector<TextureHandle> textureIDMap{GLOBAL_ALLOC};
};
static PendingSceneLoad* sPendingSceneLoad = nullptr;

static void DestroyPendingSceneLoad()
{
    if (!sPendingSceneLoad)
        return;
    Destruct(GLOBAL_ALLOC, sPendingSceneLoad);
    sPendingSceneLoad = nullptr;
}

// Routes a matching HDRI to load after the in-flight scene finishes installing. Falls
// back to loading immediately if no scene load is pending (e.g. it failed to start).
static void DeferEnvMapForPendingLoad(String const& envMapPath)
{
    if (sPendingSceneLoad)
        sPendingSceneLoad->envMapPath = envMapPath;
    else
        LoadEnvMap(envMapPath);
}

static void FinishPendingSceneLoad()
{
    if (!sPendingSceneLoad)
        return;
    PendingSceneLoad& load = *sPendingSceneLoad;
    String envMapPath = load.envMapPath;
    bool installed = false;
    try
    {
        load.stats.uploadMs = MillisecondsSince(load.loadStart);
        // Publish the new scene's geometry/texture bindings now that it's about to become
        // the current scene (BuildSceneTables reads these from GEditor).
        GEditor.meshGeometry = std::move(load.meshGeometry);
        GEditor.curveGeometry = std::move(load.curveGeometry);
        GEditor.textureIDMap = std::move(load.textureIDMap);
        FinalizeSceneUpload(*load.scene, load.gpu, load.stats, *load.alloc);
        InstallLoadedScene(load.scenePayloadPath, load.gpu);
        installed = true;
        double const loadMs = MillisecondsSince(load.loadStart);
        LOG(Editor, LogInfo, "Scene load complete in {:.2f} ms, {} meshes, {} instances, {} curves, {} materials",
            loadMs, load.stats.sceneMeshCount, load.stats.sceneInstanceCount, load.stats.sceneCurveCount,
            load.stats.sceneMaterialCount);
    }
    catch (std::exception const& e)
    {
        DestroyGPUScene(load.gpu);
        LOG(Editor, LogError, "Failed to finalize scene load ({})", e.what());
    }
    catch (...)
    {
        DestroyGPUScene(load.gpu);
        LOG(Editor, LogError, "Failed to finalize scene load");
    }
    DestroyPendingSceneLoad();
    if (installed && !envMapPath.empty())
        LoadEnvMap(envMapPath);
}

// Polled once per editor frame: finalizes + installs the scene once its background
// upload drain completes. Returns true while a load is still pending.
bool PumpSceneLoad()
{
    if (!sPendingSceneLoad)
        return false;
    if (sPendingSceneLoad->gpu)
    {
        GPUScene::Result r = sPendingSceneLoad->gpu->Poll();
        if (r == GPUScene::Result::InProgress)
            return true; // still streaming; keep rendering the current/idle scene
        if (r != GPUScene::Result::Ready)
        {
            LOG(Editor, LogError, "Scene upload failed ({}); aborting load", static_cast<int>(r));
            DestroyGPUScene(sPendingSceneLoad->gpu);
            DestroyPendingSceneLoad();
            return false;
        }
    }
    FinishPendingSceneLoad();
    return false;
}

void LoadScene(StringView path)
{
    LOG(Editor, LogInfo, "Loading scene: {}", path);

    // A previous async load must complete before starting another (it owns GEditor maps
    // + the GPUScene about to be replaced).
    if (sPendingSceneLoad)
        FinishPendingSceneLoad();

    auto const loadStart = std::chrono::steady_clock::now();
    GPUScene* gpu = nullptr;
    PendingSceneLoad* load = nullptr;
    try
    {
        String scenePayloadPath = PrepareScenePayloadFile(path);
        load = Construct<PendingSceneLoad>(GLOBAL_ALLOC);
        load->scenePayloadPath = scenePayloadPath;
        load->loadStart = loadStart;
        load->arena.emplace(GLOBAL_ALLOC, kDefaultSceneLoadScratchBudget);
        load->alloc.emplace(*load->arena);
        load->file.emplace(scenePayloadPath, MemoryMappedAccess::ReadOnly);
        load->scene.emplace(*load->file, load->alloc->Ptr());
        LoadFSCN(*load->scene);

        InitializeSceneLoad(*load->scene, load->stats, gpu);
        load->gpu = gpu;
        BeginSceneUpload(*load->scene, gpu, load->meshGeometry, load->curveGeometry, load->textureIDMap);
        // Hand off to PumpSceneLoad; the worker drains while the editor keeps rendering.
        sPendingSceneLoad = load;
    }
    catch (std::exception const& e)
    {
        sPendingSceneLoad = nullptr;
        DestroyGPUScene(gpu); // joins any background drain that may hold pointers into `load`
        if (load) Destruct(GLOBAL_ALLOC, load);
        LOG(Editor, LogError, "Failed to load scene: {} ({})", path, e.what());
    }
    catch (...)
    {
        sPendingSceneLoad = nullptr;
        DestroyGPUScene(gpu);
        if (load) Destruct(GLOBAL_ALLOC, load);
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
        gpu->UploadEnvMap(tex);
        GEditor.Scene().GetSceneGlobals() = {
            .type = FSceneEnvironmentType::EnvMap,
            .color = GEditor.shaderGlobals.ambientColor,
            .strength = GEditor.shaderGlobals.ambientPower,
            .azimuthOffset = GEditor.shaderGlobals.envAzimuthOffset,
        };
        GEditor.shaderGlobals.useEnvMap = 1u;
        gpu->FillGlobals(GEditor.shaderGlobals, GContext->enableHDR);
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
        // HDRIs sharing the scene's filename load too. The scene load is async, so defer
        // the env map until the scene is installed (PumpSceneLoad applies it).
        String hdriPath = path.string().substr(0, path.string().length() - ext.length());
        String envPath;
        if (std::filesystem::exists(hdriPath + ".hdr"))
            envPath = hdriPath + ".hdr";
        else if (std::filesystem::exists(hdriPath + ".hdri"))
            envPath = hdriPath + ".hdri";
        if (!envPath.empty())
            DeferEnvMapForPendingLoad(envPath);
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
