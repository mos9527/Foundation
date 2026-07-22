#include <Core/Paths.hpp>
#include <Core/JobGraph.hpp>
#include <algorithm>
#include <filesystem>
#include <cctype>
#include <limits>
#include <numbers>

#include "EditorState.hpp"
#include <Fonts/PlexSansIcon.h>
#include <Renderer/Mesh.hpp>
#include "Renderer/GPUScene.hpp"
#include "Renderer/LightBVH.hpp"
#include "Renderer/Postprocess.hpp"
#include <Math/Decompose.hpp>
#include <Math/Quantize.hpp>
#include <imgui.h>
#include <tracy/Tracy.hpp>
#include <cmath>
static constexpr const char* kTempScenePath = "Cache/Last.fscn";

static String sDeferredScenePath;
static String sDeferredEnvMapPath;

static bool PathsReferToSameFile(StringView a, StringView b)
{
    std::error_code ec;
    if (std::filesystem::equivalent(a.data(), b.data(), ec))
        return true;
    ec.clear();
    auto const canonA = std::filesystem::weakly_canonical(a.data(), ec);
    if (ec)
        return false;
    ec.clear();
    auto const canonB = std::filesystem::weakly_canonical(b.data(), ec);
    if (ec)
        return false;
    return canonA == canonB;
}

// Drop the editor's read-only mapping when we need to rewrite the same payload file (e.g. the
// glTF import temp cache while a previous import still has it open).
static void ReleaseScenePayloadFileForRewrite(StringView payloadPath)
{
    if (!GEditor.sceneFile.has_value())
        return;
    if (!PathsReferToSameFile(GEditor.currentSavePath, payloadPath))
        return;
    GEditor.scene.reset();
    GEditor.sceneFile.reset();
}
static constexpr size_t kDefaultSceneLoadScratchBudget = 64ull * (1ull << 20);

void CommitSceneToGPU(bool resetAccumulation)
{
    if (!GContext->gpuScene || !GEditor.HasScene())
        return;
    if (resetAccumulation)
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
    ::CommitSceneToGPU(GEditor.Scene(), *GContext->gpuScene, GEditor.resources, GEditor.shaderGlobals);
}

static FTexture LoadViewLUT(StringView path, Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    FTexture texture(alloc);
    LoadDDS(texture, PathsResolve(path));
    return texture;
}

static bool HasDDSExtension(StringView path)
{
    String ext = std::filesystem::path(path.data()).extension().string();
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".dds";
}

static FTexture LoadMatcapTexture(StringView path, Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    FTexture texture(alloc);
    String resolvedPath = PathsResolve(path);
    if (HasDDSExtension(resolvedPath))
        LoadDDS(texture, resolvedPath);
    else
        LoadRGBA8(texture, resolvedPath, false);
    return texture;
}

static void LoadSelectedViewLUTs(FTexture& sdr, FTexture& hdr, int& sdrIndex, int& hdrIndex,
                                 String const& sdrExternalPath, String const& hdrExternalPath,
                                 Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    String sdrPath = Postprocess::ResolveSelectedViewLUTPath(Postprocess::ViewLUTDomain::SDR,
                                                             sdrIndex, sdrExternalPath);
    String hdrPath = Postprocess::ResolveSelectedViewLUTPath(Postprocess::ViewLUTDomain::HDR,
                                                             hdrIndex, hdrExternalPath);

    sdr = LoadViewLUT(sdrPath, alloc);
    hdr = LoadViewLUT(hdrPath, alloc);
}

static void LoadSelectedViewLUTs(FTexture& sdr, FTexture& hdr)
{
    LoadSelectedViewLUTs(sdr, hdr, GEditor.viewLUTSdrIndex, GEditor.viewLUTHdrIndex,
                         GEditor.viewLUTSdrExternalPath, GEditor.viewLUTHdrExternalPath);
}

static void UploadEditorViewLUTs(GPUScene* gpu, FTexture const& sdr, FTexture const& hdr)
{
    CHECK(gpu != nullptr);
    GPUScene::Result sdrResult = gpu->Upload(sdr, GEditor.viewLUTSdrHandle, "View LUT SDR", true);
    GPUScene::Result hdrResult = gpu->Upload(hdr, GEditor.viewLUTHdrHandle, "View LUT HDR", true);
    CHECK_MSG(sdrResult == GPUScene::Result::Ready, "Failed to upload SDR view LUT ({})", static_cast<int>(sdrResult));
    CHECK_MSG(hdrResult == GPUScene::Result::Ready, "Failed to upload HDR view LUT ({})", static_cast<int>(hdrResult));
}

String ResolveMatcapPath(int& index, String const& externalPath)
{
    int const externalIndex = kMatcapCount; // Last entry reserved for external path
    if (index == externalIndex && !externalPath.empty())
        return externalPath;

    if (index < 0 || index >= externalIndex)
        index = 0;
    return kMatcaps[index].path;
}

static void LoadSelectedMatcap(FTexture& matcap, int& matcapIndex, String const& externalPath,
                               Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    CHECK(matcapIndex >= 0 && matcapIndex <= kMatcapCount + 1);
    String matcapPath = ResolveMatcapPath(matcapIndex, externalPath);
    matcap = LoadMatcapTexture(matcapPath, alloc);
}

static void LoadSelectedMatcap(FTexture& matcap)
{
    LoadSelectedMatcap(matcap, GEditor.matcapIndex, GEditor.matcapExternalPath);
}

static void UploadEditorMatcap(GPUScene* gpu, FTexture const& matcap)
{
    CHECK(gpu != nullptr);
    GPUScene::Result matcapResult = gpu->Upload(matcap, GEditor.matcapHandle, "Matcap", true);
    CHECK_MSG(matcapResult == GPUScene::Result::Ready, "Failed to upload matcap ({})", static_cast<int>(matcapResult));
    GEditor.shaderGlobals.matcapTextureIndex = GEditor.matcapHandle.index;
}

static double MillisecondsSince(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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
        UploadEditorViewLUTs(GContext->gpuScene, sdr, hdr);
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

    return true;
}

bool ApplyMatcapSelection()
{
    if (!GContext->gpuScene)
    {
        LOG(Editor, LogWarn, "Ignoring matcap selection before a scene is loaded");
        return false;
    }
    try
    {
        FTexture matcap(GLOBAL_ALLOC);
        LoadSelectedMatcap(matcap);
        UploadEditorMatcap(GContext->gpuScene, matcap);
    }
    catch (std::exception const& e)
    {
        LOG(Editor, LogError, "Failed to apply matcap selection: {}", e.what());
        return false;
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to apply matcap selection");
        return false;
    }
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

static Vector<float> CombineRenderTextures(Span<const FTexture> textures, uint32_t& outWidth, uint32_t& outHeight,
                                         Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    CHECK_MSG(!textures.empty(), "Invalid render texture count");
    auto const format = textures[0].GetFormat();
    CHECK_MSG(format == RHIResourceFormat::R32G32B32A32SignedFloat ||
                  format == RHIResourceFormat::R16G16B16A16SignedFloat,
              "Invalid render texture format for readback combine (got {}). RGBA16F or RGBA32F is expected.",
              format);
    for (FTexture const& texture : textures)
    {
        uint32_t width = texture.GetWidth();
        uint32_t height = texture.GetHeight();
        CHECK_MSG(texture.GetFormat() == format,
                  "Mismatched render readback texture format (got {}, expected {})",
                  texture.GetFormat(), format);
        CHECK_MSG(width == outWidth && height == outHeight, "Mismatched render readback texture extents (got {}x{}, expected {}x{})", width, height, outWidth, outHeight);
    }
    size_t const pixelCount = static_cast<size_t>(outWidth) * outHeight;
    size_t const componentCount = pixelCount * 4;
    Vector<float> combined(componentCount, alloc);
    for (FTexture const& texture : textures)
    {
        if (format == RHIResourceFormat::R32G32B32A32SignedFloat)
        {
            auto const* rgba = reinterpret_cast<const float*>(texture.bytes.data());
            for (size_t i = 0; i < componentCount; ++i)
                combined[i] += rgba[i];
        }
        else
        {
            auto const* rgba16 = reinterpret_cast<const uint16_t*>(texture.bytes.data());
            for (size_t i = 0; i < componentCount; ++i)
                combined[i] += Math::dequantizeFP16(rgba16[i]);
        }
    }
    for (size_t i = 0; i < pixelCount; ++i)
        combined[i * 4 + 3] = 1.0f;
    return combined;
}

void DoRenderReadback(RendererOutputs const& outputs)
{
    auto* renderer = GContext->renderer;
    uint32_t w = 0;
    uint32_t h = 0;

    if (GEditor.renderTask.format == ERenderFormat::HDR)
    {
        Vector<FTexture> hdrTextures(GLOBAL_ALLOC);
        auto PushHDR = [&](ResourceHandle handle)
        {
            if (handle == kInvalidHandle)
                return;
            auto* texture = renderer->DerefResource(handle).Get<RHITexture*>();
            hdrTextures.push_back(ReadbackRenderTexture(texture, GLOBAL_ALLOC));
        };
        PushHDR(outputs.diffuse);
        if (outputs.specular != kInvalidHandle && outputs.specular != outputs.diffuse)
            PushHDR(outputs.specular);
        CHECK_MSG(!hdrTextures.empty(), "Invalid HDR readback outputs");
        // AOVs are provided (without alpha), combine them into one HDR buffer
        w = hdrTextures[0].GetWidth(), h = hdrTextures[0].GetHeight();
        const Vector<float> combined = CombineRenderTextures(hdrTextures, w, h);
        const char* hdrPath = GEditor.renderTask.outputPath.c_str();
        SaveHDR(combined.data(), static_cast<int>(w), static_cast<int>(h), hdrPath);
        LOG(Editor, LogInfo, "{} HDR image saved to {} ({}x{}, {} frames)",
            GEditor.rendererMode == ERendererMode::PathTracer ? "Path tracer" : "Raster", hdrPath, w, h,
            GEditor.shaderGlobals.ptAccumulatedFrames);
    } else
    {
        CHECK_MSG(GEditor.postprocessOutput != kInvalidHandle, "Invalid SDR readback texture");
        auto sdrTexture = renderer->DerefResource(GEditor.postprocessOutput).Get<RHITexture*>();
        const FTexture sdr = ReadbackRenderTexture(sdrTexture, GLOBAL_ALLOC);        
        const char* sdrPath = GEditor.renderTask.outputPath.c_str();
        // Output as is
        CHECK_MSG(sdr.GetFormat() == RHIResourceFormat::R8G8B8A8Unorm, "Invalid SDR readback texture format. Is HDR currently enabled?");
        w = sdr.GetWidth(), h = sdr.GetHeight();
        SavePNG(sdr.bytes.data(), static_cast<int>(w), static_cast<int>(h), sdrPath);
        LOG(Editor, LogInfo, "{} SDR image saved to {} ({}x{}, {} frames)",
            GEditor.rendererMode == ERendererMode::PathTracer ? "Path tracer" : "Raster", sdrPath, w, h,
            GEditor.shaderGlobals.ptAccumulatedFrames);
    }
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
    int const idx = SceneInstanceIndexFromId(GEditor.selectedInstance);
    auto& sceneInstances = GEditor.Scene().mTables.instances;
    if (idx < 0 || idx >= static_cast<int>(sceneInstances.size()))
        return;

    if (GEditor.animation)
        GEditor.animation->RemoveInstance(static_cast<uint32_t>(idx), GEditor.resources);
    sceneInstances.erase(sceneInstances.begin() + idx);
    GEditor.Scene().RebuildIndex();
    ClearSelection();
    // Recommit the (now smaller) instance table, then reclaim geometry no longer
    // referenced. Wait for the GPU to finish before reclaiming resident resources.
    CommitSceneToGPU(true);
    GContext->device->WaitIdle();
    GContext->gpuScene->Collect();
}

static void ApplySceneCamera(FImportedScene const& scene, FArcballCamera& cameraState,
                             CameraApertureState& apertureState, RendererUBO& globals)
{
    auto cameras = scene.GetCameras();
    if (cameras.empty())
        return;

    auto& camera = cameras.front();
    static constexpr float kSceneCameraOrbitRadius = 1.0f;
    vec3 dir = camera.transform.rotation * vec3(0, 0, 1);
    cameraState.rot = camera.transform.rotation;
    cameraState.center = camera.transform.transform - dir * kSceneCameraOrbitRadius;
    cameraState.radius = kSceneCameraOrbitRadius;
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
           size_t(desc.instanceBudget) * sizeof(GSInstance) +
           size_t(desc.materialBudget) * sizeof(GSMaterial) +
           size_t(desc.lightBudget) * sizeof(GSLight) +
           size_t(desc.lightBudget) * 2u * sizeof(GSLightBVHNode) +
           size_t(desc.lightBudget) * sizeof(uint32_t) +
           size_t(desc.lightBudget) * sizeof(uint2) +
           size_t(desc.lightBudget) * sizeof(uint32_t) +
           size_t(desc.lightBudget) * 2u * sizeof(uint32_t) +
           size_t(desc.tlasInstanceBudget) * GContext->device->WriteAccelerationStructureInstanceData({}, nullptr) +
           size_t(desc.tlasBudget) +
           size_t(desc.tlasScratchBudget) +
           size_t(desc.dynamicGeometryBudget) +
           size_t(desc.dynamicStagingBudget) * (size_t(desc.dynamicStagingFramesInFlight) + 1u);
}

static GPUScene* CreateGPUScene(FImportedScene const& scene, size_t& outBudgetBytes)
{
    auto estimatedBudget = CalculateSceneGPUDesc(scene, GContext->device->GetCapabilities());
    LOG(Editor, LogDebug,
        "Estimated GPUScene budget: primitive {} MB, dynamic {} MB, instances {}, TLAS instances {}, materials {}, lights {}, textures {}",
        estimatedBudget.primitiveBudget / (1u << 20),
        estimatedBudget.dynamicGeometryBudget / (1u << 20),
        estimatedBudget.instanceBudget,
        estimatedBudget.tlasInstanceBudget,
        estimatedBudget.materialBudget,
        estimatedBudget.lightBudget,
        estimatedBudget.texturesBudget);

    FAnimationRuntimeBudget animationBudget = CalculateAnimationRuntimeBudget(scene);
    outBudgetBytes = GPUSceneBudgetBytes(estimatedBudget) + animationBudget.TotalBytes();
    if (animationBudget.TotalBytes() != 0)
        LOG(Editor, LogDebug, "Animation buffers: {:.2f} MB input/staging, {:.2f} MB palettes",
            animationBudget.inputBytes * 2u / static_cast<float>(1u << 20),
            animationBudget.paletteBytes / static_cast<float>(1u << 20));

    auto* gpu = Construct<GPUScene>(GContext->allocator, GContext->device.Get(), GContext->allocator,
                                    estimatedBudget, GContext->editorFrameScratch.get());
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
    ReleaseScenePayloadFileForRewrite(scenePayloadPath);
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
    GEditor.viewLUTSdrHandle = {};
    GEditor.viewLUTHdrHandle = {};
    GEditor.matcapIndex = {};
    GEditor.matcapExternalPath.clear();
    GEditor.matcapHandle = {};
    GEditor.shaderGlobals.matcapTextureIndex = UINT32_MAX;
}

static void InitializeSceneLoad(FImportedScene& scene, SceneLoadStats& stats, GPUScene*& newGPUScene)
{
    stats.sceneMeshCount = scene.GetMeshes().size();
    stats.sceneInstanceCount = scene.GetInstances().size();
    stats.sceneCurveCount = scene.GetCurves().size();
    stats.sceneMaterialCount = scene.GetMaterials().size();
    newGPUScene = CreateGPUScene(scene, stats.sceneGpuBudget);
}

static void BeginSceneUpload(FImportedScene& scene, GPUScene* gpu, FSceneGPUResources& resources)
{
    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    UploadSceneResources(scene, *gpu, resources);
}

static void PrepareSceneGlobals(FImportedScene& scene, GPUScene* gpu, AllocatorStack& sceneAlloc)
{
    ApplySceneGlobals(scene);
    if (FLight const* environment = scene.GetEnvironmentLight(); environment && environment->HasEnvironmentTexture())
        UploadSceneEnvironment(scene, *environment, *gpu);
    {
        FTexture sdr(&sceneAlloc);
        FTexture hdr(&sceneAlloc);
        LoadSelectedViewLUTs(sdr, hdr, GEditor.viewLUTSdrIndex, GEditor.viewLUTHdrIndex,
                             GEditor.viewLUTSdrExternalPath, GEditor.viewLUTHdrExternalPath, &sceneAlloc);
        UploadEditorViewLUTs(gpu, sdr, hdr);
    }
    {
        FTexture matcap(&sceneAlloc);
        LoadSelectedMatcap(matcap, GEditor.matcapIndex, GEditor.matcapExternalPath, &sceneAlloc);
        UploadEditorMatcap(gpu, matcap);
    }

    ApplySceneCamera(scene, GEditor.camera, GEditor.aperture, GEditor.shaderGlobals);
    gpu->UpdateUBO(GEditor.shaderGlobals);
}

static void InstallLoadedScene(String const& scenePayloadPath, GPUScene*& newGPUScene)
{
    GContext->device->WaitIdle();
    DestroyEditorRenderer(GContext);
    GEditor.animation.reset();
    DestroyGPUScene(GContext->gpuScene);
    GContext->gpuScene = newGPUScene;
    newGPUScene = nullptr;
    GEditor.OpenSceneFile(scenePayloadPath);
    GEditor.animation.emplace(GContext->allocator);
    GEditor.animation->Initialize(GEditor.Scene(), *GContext->gpuScene, GContext->device.Get(), GEditor.resources);
    ClearSelection();
    GEditor.cameraUpdated = true;
    GEditor.state = FERunningEnter;
}

struct PendingSceneLoad
{
    String scenePayloadPath;
    Optional<ScopedArena> arena;
    Optional<AllocatorStack> alloc;
    Optional<MemoryMappedFile> file;
    Optional<FImportedScene> scene;
    SceneLoadStats stats;
    std::chrono::steady_clock::time_point loadStart;
    String envMapPath; // optional HDRI to load once the scene finishes streaming
    FSceneGPUResources resources; // published into GEditor at install
};
static PendingSceneLoad* sPendingSceneLoad = nullptr;

static void DestroyPendingSceneLoad()
{
    if (!sPendingSceneLoad)
        return;
    Destruct(GLOBAL_ALLOC, sPendingSceneLoad);
    sPendingSceneLoad = nullptr;
}

static void FinishPendingSceneLoad()
{
    if (!sPendingSceneLoad)
        return;
    if (GContext->gpuScene)
        GContext->gpuScene->Join();
    DestroyPendingSceneLoad();
}

bool PollSceneLoad()
{
    if (!sPendingSceneLoad)
        return false;
    CHECK(GContext->gpuScene);
    GPUScene::Result r = GContext->gpuScene->Poll();
    if (r == GPUScene::Result::InProgress)
    {
        // Re-commit so newly resident textures replace their defaults; geometry streams into
        // the TLAS automatically (the per-frame TLAS pass only writes Ready instances).
        CommitSceneToGPU(true);
        return true;
    }

    String envMapPath = sPendingSceneLoad->envMapPath;
    if (r == GPUScene::Result::Ready)
    {
        CommitSceneToGPU(true);
        double const loadMs = MillisecondsSince(sPendingSceneLoad->loadStart);
        LOG(Editor, LogInfo, "Scene streamed in {:.2f} ms, {} meshes, {} instances, {} curves, {} materials",
            loadMs, sPendingSceneLoad->stats.sceneMeshCount, sPendingSceneLoad->stats.sceneInstanceCount,
            sPendingSceneLoad->stats.sceneCurveCount, sPendingSceneLoad->stats.sceneMaterialCount);
    }
    else
    {
        LOG(Editor, LogError, "Scene stream failed ({}); the partially loaded scene stays installed",
            static_cast<int>(r));
    }
    DestroyPendingSceneLoad(); // Poll() already joined the worker, so the backing memory is free to release
    if (r == GPUScene::Result::Ready && !envMapPath.empty())
        LoadEnvMap(envMapPath);
    return false;
}

void RequestLoadScene(StringView path, StringView envMapPath)
{
    sDeferredScenePath = path;
    sDeferredEnvMapPath = envMapPath;
}

void CheckDeferredSceneLoad()
{
    if (sDeferredScenePath.empty())
        return;
    String path = std::move(sDeferredScenePath);
    String envMapPath = std::move(sDeferredEnvMapPath);
    sDeferredScenePath.clear();
    sDeferredEnvMapPath.clear();
    LoadScene(path);
    if (!envMapPath.empty())
        if (sPendingSceneLoad)
            sPendingSceneLoad->envMapPath = envMapPath;
        else
            LoadEnvMap(envMapPath);
}

void LoadScene(StringView path)
{
    LOG(Editor, LogInfo, "Loading scene: {}", path);

    // A previous streaming load must complete before starting another (it owns GEditor maps
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
        // Globals/camera + env map + view LUTs first (small, synchronous); then queue the
        // heavy geometry/textures for the background drain.
        PrepareSceneGlobals(*load->scene, gpu, *load->alloc);
        BeginSceneUpload(*load->scene, gpu, load->resources);

        // Publish the handle maps and install the scene immediately so it renders while the
        // queued uploads stream in (PollSceneLoad drives the drain + re-commits).
        GEditor.resources = std::move(load->resources);
        InstallLoadedScene(load->scenePayloadPath, gpu);
        CommitSceneToGPU(true);
        sPendingSceneLoad = load;
    }
    catch (std::exception const& e)
    {
        sPendingSceneLoad = nullptr;
        DestroyGPUScene(gpu);
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
        gpu->UploadEnvironmentMap(tex);
        FLight& environment = GEditor.Scene().EnsureEnvironmentLight();
        environment.environmentMap = true;
        gpu->UpdateUBO(GEditor.shaderGlobals);
        UpdateSceneLights();
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
        // HDRIs sharing the scene's filename load too. The scene load is async, so defer
        // the env map until the scene is installed (PollSceneLoad applies it).
        String hdriPath = path.string().substr(0, path.string().length() - ext.length());
        String envPath;
        if (std::filesystem::exists(hdriPath + ".hdr"))
            envPath = hdriPath + ".hdr";
        else if (std::filesystem::exists(hdriPath + ".hdri"))
            envPath = hdriPath + ".hdri";
        RequestLoadScene(filePath, envPath);
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
