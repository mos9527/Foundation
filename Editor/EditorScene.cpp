#include <algorithm>
#include <cctype>
#include <limits>
#include <numbers>

#include "PathUtil.hpp"
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

using namespace EditorPathUtil;

static constexpr const char* kTempScenePath = "Cache/Last.fscn";

enum class SceneRequestKind { Replace, Append, Environment };
struct SceneLoadRequest
{
    SceneRequestKind kind;
    String path;
    String envMapPath;
};
static Vector<SceneLoadRequest> sSceneRequests{GLOBAL_ALLOC};
static Vector<String> sImportedSceneCachePaths{GLOBAL_ALLOC};

// Compare generated cache paths lexically, including relative spellings and separators.
static bool PathsReferToSameFile(StringView a, StringView b)
{
    auto& app = *GContext->application;
    return NormalizePath(app.ResolveRelativePathBase(a)) == NormalizePath(app.ResolveRelativePathBase(b));
}

// Only generated import caches are removed, and only after all metadata/payload views close.
static void ReleaseScenePayloadFiles(StringView preservePath = {})
{
    GEditor.scene.reset();
    GEditor.sceneFile.reset();
    std::erase_if(sImportedSceneCachePaths, [&](String const& path)
    {
        if (!preservePath.empty() && PathsReferToSameFile(path, preservePath))
            return false;
        if (!GContext->application->RemoveFile(path))
            LOG(Editor, LogWarn, "Could not remove imported scene cache {}", path);
        return true;
    });
}

// Replace-loads may reuse Last.fscn. Additive imports always use their own payload file.
static void ReleaseScenePayloadFileForRewrite(StringView payloadPath)
{
    if (GEditor.sceneFile && PathsReferToSameFile(GEditor.currentSavePath, payloadPath))
        ReleaseScenePayloadFiles();
}
static constexpr size_t kDefaultSceneLoadScratchBudget = 64ull * (1ull << 20);

void CommitSceneToGPU(bool resetAccumulation)
{
    if (!GContext->gpuScene || !GEditor.HasScene())
        return;
    if (resetAccumulation)
        GEditor.shaderGlobals.accumulatedFrames = 0;
    GPUSceneUpdateFlags flags{};
    if (GEditor.rendererConfig.ptLightBVHIncludeEmissiveClusters)
        flags |= GPUSceneUpdateFlagsBits::LBVH_IncludeEmissiveClusters;
    ::CommitSceneToGPU(GEditor.Scene(), *GContext->gpuScene, GEditor.resources, GEditor.shaderGlobals, flags);
}

static FTexture LoadViewLUT(StringView path, Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    FTexture texture(alloc);
    LoadDDS(texture, GContext->application->ResolveRelativePathBase(path));
    return texture;
}

static bool HasDDSExtension(StringView path)
{
    return LowerExtension(path) == ".dds";
}

static FTexture LoadMatcapTexture(StringView path, Allocator* alloc = GLOBAL_ALLOC)
{
    CHECK(alloc != nullptr);
    FTexture texture(alloc);
    String resolvedPath = GContext->application->ResolveRelativePathBase(path);
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
    FTexture sdr(GLOBAL_ALLOC);
    FTexture hdr(GLOBAL_ALLOC);
    LoadSelectedViewLUTs(sdr, hdr);
    UploadEditorViewLUTs(GContext->gpuScene, sdr, hdr);
    return true;
}

bool ApplyMatcapSelection()
{
    if (!GContext->gpuScene)
    {
        LOG(Editor, LogWarn, "Ignoring matcap selection before a scene is loaded");
        return false;
    }
    FTexture matcap(GLOBAL_ALLOC);
    LoadSelectedMatcap(matcap);
    UploadEditorMatcap(GContext->gpuScene, matcap);
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
            RendererModeDisplayName(GEditor.rendererMode), hdrPath, w, h,
            GEditor.shaderGlobals.accumulatedFrames);
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
            RendererModeDisplayName(GEditor.rendererMode), sdrPath, w, h,
            GEditor.shaderGlobals.accumulatedFrames);
    }
}

void UpdateSceneLights()
{
    if (!GEditor.HasScene() || !GContext->gpuScene)
        return;
    CommitSceneToGPU(false);
}

void DeleteSceneNode(FUUID nodeId)
{
    if (!GContext->gpuScene || !GEditor.HasScene() || !GEditor.Scene().CanRemoveNode(nodeId))
        return;
    auto& scene = GEditor.Scene();
    // Keep per-instance geometry overrides aligned when an entire subtree disappears.
    HashMap<FUUID, GeometryHandle> overrides(GLOBAL_ALLOC);
    auto const instances = scene.GetInstances();
    for (size_t i = 0; i < instances.size() && i < GEditor.resources.instanceGeometry.size(); ++i)
        if (GEditor.resources.instanceGeometry[i].IsValid())
            overrides.emplace(instances[i].id, GEditor.resources.instanceGeometry[i]);
    bool const selectionRemoved = scene.IsNodeInSubtree(GEditor.selectedNode, nodeId);
    if (!scene.RemoveNode(nodeId))
        return;
    GEditor.resources.instanceGeometry.assign(scene.GetInstances().size(), GeometryHandle{});
    for (size_t i = 0; i < scene.GetInstances().size(); ++i)
    {
        auto const existing = overrides.find(scene.GetInstances()[i].id);
        if (existing != overrides.end())
            GEditor.resources.instanceGeometry[i] = existing->second;
    }
    if (selectionRemoved)
        ClearSelection();
    // Recommit components before reclaiming geometry no longer referenced by any node.
    CommitSceneToGPU(true);
    GContext->device->WaitIdle();
    GContext->gpuScene->Collect();
}

void DeleteSelectedNode()
{
    DeleteSceneNode(GEditor.selectedNode);
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

static GPUScene* CreateGPUScene(FImportedScene const& scene)
{
    auto desc = CalculateSceneGPUDesc(scene, GContext->device->GetCapabilities());
    LOG(Editor, LogDebug, "GPUScene pages: primitive {} KiB, dynamic {} KiB, upload {} KiB; {} texture descriptors",
        desc.primitivePageSize / 1024u, desc.dynamicGeometryPageSize / 1024u, desc.uploadPageSize / 1024u,
        desc.texturesBudget);
    return Construct<GPUScene>(GContext->allocator, GContext->device.Get(), GContext->jobs.get(), GContext->allocator,
                                desc, GContext->editorFrameScratch.get());
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

static String PrepareScenePayloadFile(StringView path, bool additive = false)
{
    auto ext = LowerExtension(path);
    if (ext == ".fscn")
        return String(path);

    // GLTF import would commit to a file first too.
    // In this way we don't need to have the whole scene allocated in memory
    // explicitly - and works the same way as loading from FSCN.
    auto& app = *GContext->application;
    String scenePayloadPath;
    if (additive)
    {
        char id[40];
        scenePayloadPath = app.ResolveRelativePathBase(
            Format("Cache/Imports/{}.fscn", FUUID::Generate().Format(id, sizeof(id))));
    }
    else
    {
        scenePayloadPath = app.ResolveRelativePathBase(kTempScenePath);
        ReleaseScenePayloadFileForRewrite(scenePayloadPath);
    }
    if (auto info = app.QueryFileInfo(scenePayloadPath); info.has_value() && info.Get().isDirectory)
        CHECK_MSG(app.RemoveDirectory(scenePayloadPath),
                  "Failed to remove stale scene cache directory {}", scenePayloadPath);
    if (String const parent{ParentPath(scenePayloadPath)}; !parent.empty())
        app.CreateDirectory(parent);
    Allocator* importScratch = GLOBAL_ALLOC;
    MemoryMappedFile sceneFile(scenePayloadPath, 64ull * 1024ull * 1024ull /* grows on demand */);
    FImportedScene writeScene(sceneFile, importScratch);
    LoadScene(*GContext->application, GContext->jobs.get(), path, writeScene, importScratch);
    return scenePayloadPath;
}

struct SceneLoadStats
{
    size_t sceneMeshCount = 0;
    size_t sceneInstanceCount = 0;
    size_t sceneCurveCount = 0;
    size_t sceneMaterialCount = 0;
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
    newGPUScene = CreateGPUScene(scene);
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
    DestroyGPUScene(GContext->gpuScene);
    GContext->gpuScene = newGPUScene;
    newGPUScene = nullptr;
    ReleaseScenePayloadFiles(scenePayloadPath);
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
    String envMapPath; // optional HDRI for replace-loads only
    bool additive{false};
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
    GContext->gpuScene->Join();
    String envMapPath = sPendingSceneLoad->envMapPath;
    CommitSceneToGPU(true);
    double const loadMs = MillisecondsSince(sPendingSceneLoad->loadStart);
    LOG(Editor, LogInfo, "{} streamed in {:.2f} ms, {} meshes, {} instances, {} curves, {} materials",
        sPendingSceneLoad->additive ? "Imported subtree" : "Scene", loadMs,
        sPendingSceneLoad->stats.sceneMeshCount, sPendingSceneLoad->stats.sceneInstanceCount,
        sPendingSceneLoad->stats.sceneCurveCount, sPendingSceneLoad->stats.sceneMaterialCount);
    DestroyPendingSceneLoad(); // Join completed all reads of the pending source metadata.
    if (!envMapPath.empty())
        LoadEnvMap(envMapPath);
    return false;
}

static void EnsureAppendedTextureCapacity(size_t firstTexture)
{
    auto* gpu = GContext->gpuScene;
    BindlessPool* pools[] = {gpu->GetTexture2DPool(), gpu->GetTexture3DPool()};
    size_t added[2]{};
    auto const textures = GEditor.Scene().GetTextures();
    for (size_t i = firstTexture; i < textures.size(); ++i)
    {
        if (!textures[i].IsValid() || IsSceneEnvironmentTexture(GEditor.Scene(), i))
            continue;
        ++added[textures[i].GetViewDimension() == RHITextureDimension::E3D ? 1 : 0];
    }
    uint32_t required[2]{};
    bool grow = false;
    for (size_t i = 0; i < 2; ++i)
    {
        auto const stats = pools[i]->GetStats();
        // Leave room for an HDRI and editor-managed textures after this import, too.
        size_t const capacity = added[i] ? stats.activeBindings + added[i] + 8u : stats.capacity;
        CHECK_MSG(capacity <= UINT32_MAX, "Appended texture descriptors exceed uint32_t");
        required[i] = static_cast<uint32_t>(capacity);
        grow |= required[i] > stats.capacity;
    }
    if (!grow)
        return;
    // Layouts may change, but texture resources/indices and the paged GPUScene stay intact.
    GContext->device->WaitIdle();
    DestroyEditorRenderer(GContext);
    for (size_t i = 0; i < 2; ++i)
        pools[i]->Reserve(required[i]);
    GEditor.state = FERunningEnter;
}

static void AppendScene(StringView path)
{
    CHECK(GEditor.HasScene() && GContext->gpuScene);
    // GPU upload jobs retain pointers to resource descriptors. Finish them before the
    // destination's metadata vectors can grow; prior geometry/texture handles remain valid.
    FinishPendingSceneLoad();
    GContext->gpuScene->Join();
    LOG(Editor, LogInfo, "Appending scene: {}", path);
    auto const loadStart = std::chrono::steady_clock::now();
    String const payloadPath = PrepareScenePayloadFile(path, true);
    auto file = ConstructUnique<MemoryMappedFile>(GLOBAL_ALLOC, payloadPath, MemoryMappedAccess::ReadOnly);
    auto& scene = GEditor.Scene();
    size_t const firstTexture = scene.GetTextures().size();
    SceneLoadStats const previous{scene.GetMeshes().size(), scene.GetInstances().size(), scene.GetCurves().size(),
                                  scene.GetMaterials().size()};
    FUUID const root = scene.AppendScene(std::move(file), Stem(path));
    if (LowerExtension(path) != ".fscn")
        sImportedSceneCachePaths.push_back(payloadPath);
    EnsureAppendedTextureCapacity(firstTexture);
    AppendSceneResources(scene, *GContext->gpuScene, GEditor.resources);
    CommitSceneToGPU(true);
    SelectNode(root);
    // Refresh feature-dependent passes (for example, adding the first curve to a raster
    // scene) and transient render resources, without replacing the resident GPUScene.
    GEditor.state = FERunningEnter;

    auto* load = Construct<PendingSceneLoad>(GLOBAL_ALLOC);
    load->scenePayloadPath = payloadPath;
    load->loadStart = loadStart;
    load->additive = true;
    load->stats.sceneMeshCount = scene.GetMeshes().size() - previous.sceneMeshCount;
    load->stats.sceneInstanceCount = scene.GetInstances().size() - previous.sceneInstanceCount;
    load->stats.sceneCurveCount = scene.GetCurves().size() - previous.sceneCurveCount;
    load->stats.sceneMaterialCount = scene.GetMaterials().size() - previous.sceneMaterialCount;
    // The active scene owns appended descriptors and payloads, not this completion marker.
    sPendingSceneLoad = load;
}

void RequestLoadScene(StringView path, StringView envMapPath)
{
    if (!path.empty())
        sSceneRequests.push_back({SceneRequestKind::Replace, String(path), String(envMapPath)});
}

void RequestAppendScene(StringView path, StringView envMapPath)
{
    if (!path.empty())
        sSceneRequests.push_back({SceneRequestKind::Append, String(path), String(envMapPath)});
}

void CheckDeferredSceneLoad()
{
    // One request per frame, in drop order. Do not mutate metadata still used by uploads
    // or replace a render graph while an explicit render/readback task is active.
    if (sSceneRequests.empty() || sPendingSceneLoad || GEditor.state == FERendering ||
        GEditor.state == FERenderingEnter)
        return;
    SceneLoadRequest request = std::move(sSceneRequests.front());
    sSceneRequests.erase(sSceneRequests.begin());
    auto const info = GContext->application->QueryFileInfo(request.path);
    if (!info || info->isDirectory)
    {
        LOG(Editor, LogWarn, "Cannot load scene or image file: {}", request.path);
        return;
    }
    if (request.kind == SceneRequestKind::Environment)
    {
        LoadEnvMap(request.path);
        return;
    }
    if (request.kind == SceneRequestKind::Append && GEditor.HasScene())
    {
        AppendScene(request.path);
        return; // Never import source exposure, view settings, camera, or HDRI over the current scene.
    }
    LoadScene(request.path);
    if (!request.envMapPath.empty())
    {
        if (sPendingSceneLoad)
            sPendingSceneLoad->envMapPath = request.envMapPath;
        else
            LoadEnvMap(request.envMapPath);
    }
}

void ShutdownSceneLoading()
{
    FinishPendingSceneLoad();
    if (GContext->gpuScene)
        GContext->gpuScene->Join();
    sSceneRequests.clear();
    ReleaseScenePayloadFiles();
    GEditor.currentSavePath.clear();
    ClearSelection();
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
    String scenePayloadPath = PrepareScenePayloadFile(path);
    load = Construct<PendingSceneLoad>(GLOBAL_ALLOC);
    load->scenePayloadPath = scenePayloadPath;
    load->loadStart = loadStart;
    load->arena.emplace(GLOBAL_ALLOC, kDefaultSceneLoadScratchBudget);
    load->alloc.emplace(*load->arena);
    load->file.emplace(scenePayloadPath, MemoryMappedAccess::ReadOnly);
    // Resident component/node metadata grows with the scene, not the fixed LUT scratch arena.
    load->scene.emplace(*load->file, GLOBAL_ALLOC);
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

void LoadEnvMap(StringView path)
{
    LOG(Editor, LogInfo, "Loading HDRI env map: {}", path);
    auto* gpu = GContext->gpuScene;
    if (!gpu)
    {
        LOG(Editor, LogWarn, "Ignoring HDRI env map load before a scene is loaded");
        return;
    }
    FTexture tex(GLOBAL_ALLOC);
    LoadHDR(tex, path);
    gpu->UploadEnvironmentMap(tex);
    FLight& environment = GEditor.Scene().EnsureEnvironmentLight();
    environment.environmentMap = true;
    gpu->UpdateUBO(GEditor.shaderGlobals);
    UpdateSceneLights();
    GEditor.shaderGlobals.accumulatedFrames = 0;
    LOG(Editor, LogInfo, "HDRI env map loaded successfully");
}

void HandleFile(const char* filePath)
{
    if (!filePath || !*filePath)
        return;
    StringView pathView(filePath);
    String const ext = LowerExtension(pathView);
    if (ext == ".gltf" || ext == ".glb" || ext == ".fscn")
    {
        // Drops append by default. A first scene can adopt its sidecar HDRI, but subsequent
        // imports must not replace the scene-global environment.
        String hdriPath{pathView.substr(0, pathView.length() - ext.length())};
        String envPath;
        auto& app = *GContext->application;
        if (auto info = app.QueryFileInfo(hdriPath + ".hdr"); info.has_value() && !info.Get().isDirectory)
            envPath = hdriPath + ".hdr";
        else if (auto info = app.QueryFileInfo(hdriPath + ".hdri"); info.has_value() && !info.Get().isDirectory)
            envPath = hdriPath + ".hdri";
        RequestAppendScene(filePath, envPath);
    }
    else if (ext == ".hdr" || ext == ".hdri")
    {
        sSceneRequests.push_back({SceneRequestKind::Environment, String(filePath), {}});
    }
    else
    {
        LOG(Editor, LogWarn, "Unknown file type dropped: '{}'", filePath);
    }
}
