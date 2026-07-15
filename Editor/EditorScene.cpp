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
#include <Renderer/Animation.hpp>
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

static GPUScene::UpdateResult CommitSceneToGPU(GPUScene* gpu, FImportedScene& scene, RendererUBO& globals,
                                               bool resetAccumulation)
{
    CHECK(gpu);
    return ::CommitSceneToGPU(scene, *gpu, GEditor.resources, globals, resetAccumulation);
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
           size_t(desc.curveAABBBudget) +
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
           size_t(desc.tlasScratchBudget);
}

static GPUScene* CreateGPUScene(FImportedScene const& scene, size_t& outBudgetBytes)
{
    auto estimatedBudget = CalculateSceneGPUDesc(scene, GContext->device->GetCapabilities());
    LOG(Editor, LogDebug,
        "Estimated GPUScene budget: primitive {} MB, instances {}, TLAS instances {}, materials {}, lights {}, textures {}",
        estimatedBudget.primitiveBudget / (1u << 20),
        estimatedBudget.instanceBudget,
        estimatedBudget.tlasInstanceBudget,
        estimatedBudget.materialBudget,
        estimatedBudget.lightBudget,
        estimatedBudget.texturesBudget);

    outBudgetBytes = GPUSceneBudgetBytes(estimatedBudget);

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

// Queues every scene geometry/texture to GPUScene's work queue. The queue drains on a
// background thread (kicked by the first Poll() in PumpSceneLoad) while the editor renders
// the (already installed) scene: instances stream into the TLAS as their geometry becomes
// resident, and materials fall back to default textures until theirs land.
static void BeginSceneUpload(FImportedScene& scene, GPUScene* gpu, FSceneGPUResources& resources)
{
    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    UploadSceneResources(scene, *gpu, resources);
}

// Applies the scene's globals + camera and uploads the environment map + view LUTs. These
// are small and synchronous, so they run BEFORE the heavy geometry/textures are queued: that
// keeps the synchronous drain limited to env + LUTs and leaves the bulk of the scene to stream
// in on the background worker.
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
    gpu->BuildUBO(GEditor.shaderGlobals);
}


static void ClearAnimationRuntime(); // defined with the skinning runtime below

static void InstallLoadedScene(String const& scenePayloadPath, GPUScene*& newGPUScene)
{
    GContext->device->WaitIdle();
    ClearAnimationRuntime(); // drop runtime referencing the scene/GPUScene we're about to replace
    DestroyEditorRenderer(GContext);
    DestroyGPUScene(GContext->gpuScene);
    GContext->gpuScene = newGPUScene;
    newGPUScene = nullptr;
    GEditor.OpenSceneFile(scenePayloadPath);
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

// Routes a matching HDRI to load after the in-flight scene finishes streaming (its
// UploadEnvMap would otherwise Join the background drain). Falls back to loading
// immediately if no scene load is pending (e.g. it failed to start).
static void DeferEnvMapForPendingLoad(String const& envMapPath)
{
    if (sPendingSceneLoad)
        sPendingSceneLoad->envMapPath = envMapPath;
    else
        LoadEnvMap(envMapPath);
}

// Joins the background drain (which holds pointers into the load's mapped file) and frees
// the load's backing memory. Safe to call once Poll() has reported Ready/failed, or to
// force-complete a still-streaming load before starting another.
static void FinishPendingSceneLoad()
{
    if (!sPendingSceneLoad)
        return;
    if (GContext->gpuScene)
        GContext->gpuScene->Join();
    DestroyPendingSceneLoad();
}

// --- Skinned animation playback ---------------------------------------------------------------
// Thin editor bindings over the portable Editor/Runtime/Animation API: owns the single
// FAnimationRuntime for the installed scene and wires it to GContext's GPUScene/jobs/scratch and
// GEditor's camera/UI.
namespace
{
FAnimationRuntime sAnimation;
// Debug toggle: force serial CPU deformation. Persists across scene loads (not part of sAnimation).
bool sAnimateParallel = true;
} // namespace

static void ClearAnimationRuntime() { sAnimation = FAnimationRuntime{}; }

static void SetupAnimationRuntime()
{
    ClearAnimationRuntime();
    if (!GEditor.HasScene())
        return;
    sAnimation.Setup(GEditor.Scene(), GEditor.resources, GContext->jobs->GetParallelForConcurrency());
}

void BeginAnimationUpdate(float dt)
{
    if (sPendingSceneLoad || !GEditor.HasScene())
    {
        sAnimation.frameActive = false;
        sAnimation.frameChanged = false;
        sAnimation.frameDone = JobHandle{};
        sAnimation.frameGraph.reset();
        return;
    }
    ExecutionPolicy const policy = sAnimateParallel ? ExecutionPolicy::Par : ExecutionPolicy::Seq;
    sAnimation.Begin(GEditor.Scene(), GContext->gpuScene, dt, *GContext->jobs, GContext->editorFrameScratch.get(), policy);
}

bool EndAnimationUpdate() { return sAnimation.End(); }

bool AnimatedCameraDrivesView() { return sAnimation.CameraDrivesView(); }

// Drives the editor arcball from the animated scene camera's current transform (maintained each
// active frame by BeginAnimationUpdate's "Anim Cameras" pass). Mirrors the transform->arcball
// mapping in ApplySceneCamera. Returns true if it moved the view (so the caller resets path-tracer
// accumulation).
bool ApplyAnimatedCameraToView()
{
    FTransform xf;
    if (!GEditor.HasScene() || !sAnimation.GetCameraTransform(GEditor.Scene(), xf))
        return false;
    vec3 dir = xf.rotation * vec3(0, 0, 1);
    GEditor.camera.center = xf.transform - dir * GEditor.camera.radius;
    GEditor.camera.rot = xf.rotation;
    return true;
}

// Minimal playback panel. Only shown when the installed scene has animation. Scrubbing while paused
// requests a one-shot pose apply (BeginAnimationUpdate honors `dirty`), so a held frame still updates.
// Resolves an animation set's display name, falling back to a positional label if the source
// animation was unnamed (or its name id isn't in the string pool).
static const char* AnimationSetLabel(uint32_t index)
{
    static char fallback[32];
    if (GEditor.HasScene())
        if (const char* name = GEditor.Scene().GetName(sAnimation.animations[index].name))
            return name;
    snprintf(fallback, sizeof(fallback), "Animation %u", index);
    return fallback;
}

// Selected NLA strip for the properties editor; -1 means none. Persists across frames.
static int sSelectedTrack = -1;
static int sSelectedStrip = -1;

void FAnimationPanel()
{
    if (!sAnimation.HasData())
        return;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Animation"))
    {
        auto& tables = GEditor.Scene().mTables;

        // Track/strip mutation helpers shared by the visual timeline's context menus and the
        // numeric list below, so both views offer the same actions.
        auto addTrack = [&]() -> int
        {
            if (sAnimation.animations.empty())
                return -1;
            FNlaTrack track(GLOBAL_ALLOC);
            track.id = FUUID::Generate();
            tables.nlaTracks.push_back(std::move(track));
            sAnimation.RefreshAnimatedInstances(GEditor.Scene());
            sAnimation.dirty = true;
            return static_cast<int>(tables.nlaTracks.size() - 1);
        };
        auto addStrip = [&](int ti, int animIdx)
        {
            if (ti < 0 || ti >= static_cast<int>(tables.nlaTracks.size()))
                return;
            if (animIdx < 0 || animIdx >= static_cast<int>(sAnimation.animations.size()))
                return;
            auto const& anim = sAnimation.animations[animIdx];
            FNlaTrack& track = tables.nlaTracks[ti];
            FNlaStrip strip{};
            strip.source = anim.name;
            float start = 0.0f;
            for (auto const& s : track.strips)
                start = std::max(start, s.stripEnd);
            strip.stripStart = start;
            strip.stripEnd = start + anim.duration;
            strip.clipEnd = anim.duration;
            track.strips.push_back(std::move(strip));
            sSelectedTrack = ti;
            sSelectedStrip = static_cast<int>(track.strips.size() - 1);
            sAnimation.RefreshAnimatedInstances(GEditor.Scene());
            sAnimation.dirty = true;
        };
        auto removeTrack = [&](int ti)
        {
            if (ti < 0 || ti >= static_cast<int>(tables.nlaTracks.size()))
                return;
            tables.nlaTracks.erase(tables.nlaTracks.begin() + ti);
            if (sSelectedTrack == ti)
                sSelectedTrack = sSelectedStrip = -1;
            else if (sSelectedTrack > ti)
                --sSelectedTrack;
            sAnimation.RefreshAnimatedInstances(GEditor.Scene());
            sAnimation.dirty = true;
        };
        auto removeStrip = [&](int ti, int si)
        {
            if (ti < 0 || ti >= static_cast<int>(tables.nlaTracks.size()))
                return;
            FNlaTrack& track = tables.nlaTracks[ti];
            if (si < 0 || si >= static_cast<int>(track.strips.size()))
                return;
            track.strips.erase(track.strips.begin() + si);
            if (sSelectedTrack == ti && sSelectedStrip == si)
                sSelectedStrip = -1;
            sAnimation.RefreshAnimatedInstances(GEditor.Scene());
            sAnimation.dirty = true;
        };
        auto toggleMute = [&](int ti)
        {
            if (ti < 0 || ti >= static_cast<int>(tables.nlaTracks.size()))
                return;
            tables.nlaTracks[ti].mute = !tables.nlaTracks[ti].mute;
            sAnimation.RefreshAnimatedInstances(GEditor.Scene());
            sAnimation.dirty = true;
        };
        // Maps a flat index into a `strips` array built in track-then-strip order (as filled
        // below) back to (track index, strip-within-track index).
        auto mapFlatStrip = [&](int flat, int& outTi, int& outSi)
        {
            outTi = outSi = -1;
            for (int ti = 0, flatIdx = 0; ti < static_cast<int>(tables.nlaTracks.size()); ++ti)
                for (int si = 0; si < static_cast<int>(tables.nlaTracks[ti].strips.size()); ++si, ++flatIdx)
                    if (flatIdx == flat)
                        outTi = ti, outSi = si;
        };

        // Visual timeline: draggable/resizable strips over a shared ruler; selecting a strip drives
        // the strip editor below. Right-click a strip/track/empty space for actions; there's nothing
        // to right-click when the scene has no tracks yet, so that bootstrap case gets a plain button.
        if (tables.nlaTracks.empty())
        {
            if (ImGui::Button(PSI_PLUS_SIGN " Add Track"))
                addTrack();
        }
        else
        {
            static float sPixelsPerSecond = 40.0f;
            static float sScrollX = 0.0f;

            Vector<ImTimelineRow> rows(GLOBAL_ALLOC);
            rows.reserve(tables.nlaTracks.size());
            for (FNlaTrack const& track : tables.nlaTracks)
                rows.push_back({GEditor.Scene().GetName(track.name), track.mute});

            Vector<ImTimelineStrip> strips(GLOBAL_ALLOC);
            for (int ti = 0; ti < static_cast<int>(tables.nlaTracks.size()); ++ti)
            {
                FNlaTrack const& track = tables.nlaTracks[ti];
                for (int si = 0; si < static_cast<int>(track.strips.size()); ++si)
                {
                    FNlaStrip const& strip = track.strips[si];
                    // Deterministic color per source clip, so repeated strips of the same clip
                    // stay visually recognizable across tracks.
                    float const hue = static_cast<float>(strip.source.hi % 360ull) / 360.0f;
                    float r, g, b;
                    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, track.mute ? 0.35f : 0.75f, r, g, b);
                    strips.push_back({.row = ti,
                                       .start = strip.stripStart,
                                       .end = strip.stripEnd,
                                       .color = ImGui::GetColorU32(ImVec4(r, g, b, 1.0f)),
                                       .label = GEditor.Scene().GetName(strip.source),
                                       .selected = ti == sSelectedTrack && si == sSelectedStrip});
                }
            }

            float playhead = sAnimation.duration > 0.0f ? std::fmod(sAnimation.time, sAnimation.duration) : 0.0f;
            ImTimelineResult const tl =
                ImTimeline("nla", Span<const ImTimelineRow>(rows.data(), rows.data() + rows.size()),
                           Span<ImTimelineStrip>(strips.data(), strips.data() + strips.size()), sAnimation.duration,
                           playhead, sPixelsPerSecond, sScrollX);

            if (tl.scrubbed)
            {
                sAnimation.time = playhead;
                sAnimation.dirty = true;
            }
            if (tl.clickedRow >= 0)
            {
                sSelectedTrack = tl.clickedRow;
                sSelectedStrip = -1;
            }
            // Clicking a track title in the timeline toggles its mute state.
            if (tl.muteToggledRow >= 0)
                toggleMute(tl.muteToggledRow);
            if (tl.clickedStrip >= 0)
            {
                mapFlatStrip(tl.clickedStrip, sSelectedTrack, sSelectedStrip);
                if (tl.stripsChanged && sSelectedTrack >= 0)
                {
                    FNlaStrip& strip = tables.nlaTracks[sSelectedTrack].strips[sSelectedStrip];
                    strip.stripStart = strips[tl.clickedStrip].start;
                    strip.stripEnd = strips[tl.clickedStrip].end;
                    sAnimation.RefreshAnimatedInstances(GEditor.Scene());
                    sAnimation.dirty = true;
                }
            }

            static int sContextTrack = -1;
            static int sContextStrip = -1;
            static int sAddStripTrack = -1; // track awaiting a source pick from the Add Strip popup
            bool openStripPicker = false;
            if (tl.rightClickedBackground)
                ImGui::OpenPopup("nla_bg_ctx");
            if (tl.rightClickedRow >= 0)
            {
                sContextTrack = tl.rightClickedRow;
                ImGui::OpenPopup("nla_track_ctx");
            }
            if (tl.rightClickedStrip >= 0)
            {
                mapFlatStrip(tl.rightClickedStrip, sContextTrack, sContextStrip);
                ImGui::OpenPopup("nla_strip_ctx");
            }
            if (ImGui::BeginPopup("nla_bg_ctx"))
            {
                if (ImGui::MenuItem(PSI_PLUS_SIGN " Add Track"))
                    addTrack();
                ImGui::EndPopup();
            }
            if (ImGui::BeginPopup("nla_track_ctx"))
            {
                if (ImGui::MenuItem(PSI_PLUS_SIGN " Add Track"))
                    addTrack();
                if (ImGui::MenuItem(PSI_PLUS_SIGN " Add Strip..."))
                {
                    sAddStripTrack = sContextTrack;
                    openStripPicker = true;
                }
                bool const muted = sContextTrack >= 0 && sContextTrack < static_cast<int>(tables.nlaTracks.size()) &&
                                    tables.nlaTracks[sContextTrack].mute;
                if (ImGui::MenuItem(muted ? "Unmute Track" : "Mute Track"))
                    toggleMute(sContextTrack);
                ImGui::Separator();
                if (ImGui::MenuItem(PSI_TRASH " Remove Track"))
                    removeTrack(sContextTrack);
                ImGui::EndPopup();
            }
            if (ImGui::BeginPopup("nla_strip_ctx"))
            {
                if (ImGui::MenuItem(PSI_PLUS_SIGN " Add Track"))
                    addTrack();
                ImGui::Separator();
                if (ImGui::MenuItem(PSI_TRASH " Remove Strip"))
                    removeStrip(sContextTrack, sContextStrip);
                ImGui::EndPopup();
            }

            // Add Strip prompts for a source clip rather than assuming one; opened deferred so the
            // originating context menu has closed first.
            if (openStripPicker)
                ImGui::OpenPopup("nla_add_strip");
            if (ImGui::BeginPopup("nla_add_strip"))
            {
                ImGui::TextDisabled("Add strip from clip");
                ImGui::Separator();
                for (uint32_t i = 0; i < sAnimation.animations.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::MenuItem(AnimationSetLabel(i)))
                        addStrip(sAddStripTrack, static_cast<int>(i));
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
        }

        // Compact transport under the timeline; scrubbing the timeline ruler replaces the old Time
        // slider, so playback state lives here as small modal buttons plus a narrow Speed control.
        if (ImModalButton(sAnimation.playing ? PSI_PAUSE " Pause" : PSI_PLAY " Play", 0, 4))
            sAnimation.playing = !sAnimation.playing;
        if (ImModalButton(PSI_REPEAT " Restart", 1, 4))
        {
            sAnimation.time = 0.0f;
            sAnimation.playing = true;
            sAnimation.dirty = true;
        }
        if (ImModalButton(sAnimation.loop ? "Loop: On" : "Loop: Off", 2, 4))
            sAnimation.loop = !sAnimation.loop;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::SliderFloat("##speed", &sAnimation.speed, 0.0f, 3.0f, "%.2fx");

        // Selected strip editor: the strip-clip relationship (Strip Frame Start/End, Clip Start/End,
        // Timescale, Influence, Cyclic) plus the source animation picker.
        if (sSelectedTrack >= 0 && sSelectedStrip >= 0 &&
            sSelectedTrack < static_cast<int>(tables.nlaTracks.size()))
        {
            FNlaTrack& track = tables.nlaTracks[sSelectedTrack];
            if (sSelectedStrip < static_cast<int>(track.strips.size()))
            {
                FNlaStrip& strip = track.strips[sSelectedStrip];
                ImGui::SeparatorText("Strip");
                const char* cur = GEditor.Scene().GetName(strip.source);
                if (ImGui::BeginCombo("Source", cur ? cur : "?"))
                {
                    for (uint32_t i = 0; i < sAnimation.animations.size(); ++i)
                    {
                        bool sel = strip.source == sAnimation.animations[i].name;
                        if (ImGui::Selectable(AnimationSetLabel(i), sel))
                        {
                            strip.source = sAnimation.animations[i].name;
                            sAnimation.RefreshAnimatedInstances(GEditor.Scene());
                            sAnimation.dirty = true;
                        }
                        if (sel)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                bool changed = false;
                changed |= ImGui::DragFloat("Strip Start", &strip.stripStart, 0.01f, 0.0f, 1e6f, "%.2f s");
                changed |= ImGui::DragFloat("Strip End", &strip.stripEnd, 0.01f, 0.0f, 1e6f, "%.2f s");
                changed |= ImGui::DragFloat("Clip Start", &strip.clipStart, 0.01f, 0.0f, 1e6f, "%.2f s");
                changed |= ImGui::DragFloat("Clip End", &strip.clipEnd, 0.01f, 0.0f, 1e6f, "%.2f s");
                changed |= ImGui::DragFloat("Timescale", &strip.timeScale, 0.01f, 0.01f, 100.0f, "%.3f");
                changed |= ImGui::SliderFloat("Influence", &strip.influence, 0.0f, 1.0f, "%.2f");
                changed |= ImGui::Checkbox("Cyclic", &strip.cyclic);
                if (changed)
                {
                    if (strip.stripEnd < strip.stripStart)
                        strip.stripEnd = strip.stripStart;
                    if (strip.clipEnd < strip.clipStart)
                        strip.clipEnd = strip.clipStart;
                    if (strip.timeScale <= 0.0f)
                        strip.timeScale = 1.0f;
                    sAnimation.RefreshAnimatedInstances(GEditor.Scene());
                    sAnimation.dirty = true;
                }
            }
        }

        // Stats aggregated over all strips on non-mute tracks.
        if (ImGui::CollapsingHeader("Stats", ImGuiTreeNodeFlags_DefaultOpen))
        {
            uint32_t stripCount = 0, clipCount = 0, channelCount = 0, trackCount = 0;
            bool skin = false, rigid = false;
            for (FNlaTrack const& track : tables.nlaTracks)
            {
                if (track.mute)
                    continue;
                ++trackCount;
                for (FNlaStrip const& strip : track.strips)
                {
                    ++stripCount;
                    auto it = sAnimation.setByName.find(strip.source);
                    if (it == sAnimation.setByName.end())
                        continue;
                    FAnimationSet const& set = sAnimation.animations[it->second];
                    clipCount += static_cast<uint32_t>(set.clips.size());
                    channelCount += set.channelCount;
                    skin |= set.hasSkin;
                    rigid |= set.hasRigid;
                }
            }
            const char* kind = skin && rigid ? "skinned + rigid" : skin ? "skinned" : rigid ? "rigid" : "-";
            ImGui::TextDisabled("Tracks: %u active   Strips: %u", trackCount, stripCount);
            ImGui::TextDisabled("Timeline: %.2f s", sAnimation.duration);
            ImGui::TextDisabled("Clips: %u   Channels: %u", clipCount, channelCount);
            ImGui::TextDisabled("Drives: %s%s", kind, sAnimation.drivesCamera ? " + camera" : "");
            ImGui::TextDisabled("%zu deforming mesh(es)%s", sAnimation.meshes.size(),
                                sAnimation.HasRigid() ? ", rigid nodes" : "");
        }

        // Instances animated by the active NLA strips; click to select one in the Hierarchy.
        if (!sAnimation.animatedList.empty() &&
            ImGui::CollapsingHeader("Animated Instances", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto instances = GEditor.Scene().GetInstances();
            for (uint32_t idx : sAnimation.animatedList)
            {
                if (idx >= instances.size())
                    continue;
                char label[128];
                if (const char* name = GEditor.Scene().GetName(instances[idx].name))
                    snprintf(label, sizeof(label), "Instance %u: %s", idx, name);
                else
                    snprintf(label, sizeof(label), "Instance %u", idx);
                ImGui::PushID(static_cast<int>(idx));
                bool const isSelected = GEditor.selectedInstance == instances[idx].id;
                if (ImGui::Selectable(label, isSelected) && GContext->gpuScene)
                    SelectInstance(instances[idx].id,
                                   GEditor.Scene().GetMaterials()[GContext->gpuScene->GetInstance(idx).materialIndex].id);
                ImGui::PopID();
            }
        }

        ImGui::Checkbox("Parallel deformation", &sAnimateParallel);
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// Whether scene instance `index` is animated by the Animation window's current NLA tracks; the
// Hierarchy uses this to pulse-highlight those rows.
bool IsInstanceAnimated(uint32_t index) { return sAnimation.IsInstanceAnimated(index); }

// Pumped once per editor frame. The scene is already installed and rendering; this advances
// the background drain, re-committing each frame so geometry/textures pop into the live
// scene as they become resident. Returns true while the scene is still streaming.
bool PumpSceneLoad()
{
    if (!sPendingSceneLoad)
        return false;
    CHECK(GContext->gpuScene);
    GPUScene::Result r = GContext->gpuScene->Poll();
    if (r == GPUScene::Result::InProgress)
    {
        // Re-commit so newly resident textures replace their defaults; geometry streams into
        // the TLAS automatically (the per-frame TLAS pass only writes Ready instances).
        CommitSceneToGPU(GContext->gpuScene, GEditor.Scene(), GEditor.shaderGlobals, true);
        return true;
    }

    String envMapPath = sPendingSceneLoad->envMapPath;
    if (r == GPUScene::Result::Ready)
    {
        CommitSceneToGPU(GContext->gpuScene, GEditor.Scene(), GEditor.shaderGlobals, true);
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

void PumpDeferredSceneLoad()
{
    if (sDeferredScenePath.empty())
        return;
    String path = std::move(sDeferredScenePath);
    String envMapPath = std::move(sDeferredEnvMapPath);
    sDeferredScenePath.clear();
    sDeferredEnvMapPath.clear();
    LoadScene(path);
    if (!envMapPath.empty())
        DeferEnvMapForPendingLoad(envMapPath);
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
        // queued uploads stream in (PumpSceneLoad drives the drain + re-commits).
        GEditor.resources = std::move(load->resources);
        InstallLoadedScene(load->scenePayloadPath, gpu); // nulls `gpu`; ownership moves to GContext
        CommitSceneToGPU(GContext->gpuScene, GEditor.Scene(), GEditor.shaderGlobals, true);
        SetupAnimationRuntime(); // builds CPU skinning state from the installed scene
        sPendingSceneLoad = load;
    }
    catch (std::exception const& e)
    {
        sPendingSceneLoad = nullptr;
        DestroyGPUScene(gpu); // null after a successful install; otherwise joins + frees the new scene
        ClearAnimationRuntime();
        if (load) Destruct(GLOBAL_ALLOC, load);
        LOG(Editor, LogError, "Failed to load scene: {} ({})", path, e.what());
    }
    catch (...)
    {
        sPendingSceneLoad = nullptr;
        DestroyGPUScene(gpu);
        ClearAnimationRuntime();
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
        FLight& environment = GEditor.Scene().EnsureEnvironmentLight();
        environment.environmentMap = true;
        gpu->BuildUBO(GEditor.shaderGlobals);
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
        // the env map until the scene is installed (PumpSceneLoad applies it).
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
