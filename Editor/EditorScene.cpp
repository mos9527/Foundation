#include "EditorState.hpp"
#include "Scene/Mesh.hpp"
#include <Core/Paths.hpp>
#include <Core/ThreadPool.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <numbers>
#include <utility>

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
    dst.radius = float2(src.width, src.height);
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
            dst.dpdu = u * src.width;  // half-extent along u
            dst.dpdv = v * src.height; // half-extent along v
        }

        if (src.normalize)
        {
            float area = 1.0f;
            if (src.type == FLightType::Disk)
                area = std::numbers::pi_v<float> * src.width * src.height;
            else
                area = 4.0f * src.width * src.height;
            
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

void CommitSceneToGPU(bool resetAccumulation)
{
    if (!GContext->gpuScene)
        return;

    auto res = GContext->gpuScene->UpdateGPUScene(GEditor.doc.instances, GEditor.doc.materials, GEditor.doc.lights);
    GEditor.shaderGlobals.firstInstance = res.firstInstance;
    GEditor.shaderGlobals.numInstances  = res.numInstances;
    GEditor.shaderGlobals.firstMaterial = res.firstMaterial;
    GEditor.shaderGlobals.numMaterials  = res.numMaterials;
    GEditor.shaderGlobals.firstLight    = res.firstLight;
    GEditor.shaderGlobals.firstLightAliasTable = res.firstLightAliasTable;
    GEditor.shaderGlobals.sceneLightWeightSum = res.sceneLightWeightSum;
    if (resetAccumulation)
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
}

static FTexture LoadViewLUT(StringView path)
{
    FTexture texture(GLOBAL_ALLOC);
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

static void LoadSelectedViewLUTs(FTexture& sdr, FTexture& hdr)
{
    String sdrPath = ResolveSelectedViewLUTPath(GEditor.viewLUTSdrIndex, kViewLUTsSdr, kViewLUTSdrCount,
                                                kDefaultViewLUTSdr, GEditor.viewLUTSdrExternalPath);
    String hdrPath = ResolveSelectedViewLUTPath(GEditor.viewLUTHdrIndex, kViewLUTsHdr, kViewLUTHdrCount,
                                                kDefaultViewLUTHdr, GEditor.viewLUTHdrExternalPath);

    sdr = LoadViewLUT(sdrPath);
    hdr = LoadViewLUT(hdrPath);
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

    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
    GEditor.state = FERunningEnter;
    return true;
}

static Vector<float> ReadbackAndCombineFloatRTs(RHITexture* const* sourceTextures, uint32_t sourceCount,
                                                uint32_t& outWidth, uint32_t& outHeight)
{
    CHECK_MSG(sourceCount > 0u, "Invalid render readback texture count");
    uint32_t w = sourceTextures[0]->mDesc.extent.x;
    uint32_t h = sourceTextures[0]->mDesc.extent.y;
    for (uint32_t i = 1; i < sourceCount; ++i)
    {
        CHECK_MSG(sourceTextures[i]->mDesc.extent.x == w && sourceTextures[i]->mDesc.extent.y == h,
                  "Mismatched render readback texture extents");
    }
    outWidth = w;
    outHeight = h;

    const size_t pixelCount = static_cast<size_t>(w) * h;
    const size_t imageBytes = pixelCount * 4 * sizeof(float); // RGBA32F

    auto readbackBuf = GContext->device->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Readback,
                                                                    .hostAccess = RHIResourceHostAccess::ReadWrite,
                                                                    .coherent = true},
                                                       .usage = RHIBufferUsageBits::TransferDestination,
                                                       .size = imageBytes * sourceCount});

    {
        ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
        auto* cmd = ctx.Get();
        cmd->Begin();
        cmd->BeginTransition();
        for (uint32_t i = 0; i < sourceCount; ++i)
        {
            cmd->SetImageTransition(sourceTextures[i],
                                    {.srcAccess = RHIResourceAccessBits::ShaderRead,
                                     .dstAccess = RHIResourceAccessBits::TransferRead,
                                     .srcStage = RHIPipelineStageBits::FragmentShader,
                                     .dstStage = RHIPipelineStageBits::Transfer,
                                     .srcImgLayout = RHITextureLayout::ShaderReadOnly,
                                     .dstImgLayout = RHITextureLayout::TransferSrc,
                                     .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        }
        cmd->EndTransition();
        for (uint32_t i = 0; i < sourceCount; ++i)
        {
            cmd->CopyImageToBuffer(
                sourceTextures[i], RHITextureLayout::TransferSrc, readbackBuf.Get(),
                {{{.dstBufferOffset = static_cast<uint32_t>(i * imageBytes),
                   .srcLayer = {.aspect = RHITextureAspectFlagBits::Color},
                   .extent = {w, h, 1}}}});
        }
        cmd->BeginTransition();
        for (uint32_t i = 0; i < sourceCount; ++i)
        {
            cmd->SetImageTransition(sourceTextures[i],
                                    {.srcAccess = RHIResourceAccessBits::TransferRead,
                                     .dstAccess = RHIResourceAccessBits::ShaderRead,
                                     .srcStage = RHIPipelineStageBits::Transfer,
                                     .dstStage = RHIPipelineStageBits::FragmentShader,
                                     .srcImgLayout = RHITextureLayout::TransferSrc,
                                     .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                                     .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        }
        cmd->EndTransition();
        cmd->End();
        ctx.Submit();
        ctx.WaitIdle();
    }

    auto* mapped = readbackBuf->Map<float>();
    Vector<float> combined(pixelCount * 4, GLOBAL_ALLOC);
    for (size_t i = 0; i < pixelCount; ++i)
    {
        combined[i * 4 + 0] = 0.0f;
        combined[i * 4 + 1] = 0.0f;
        combined[i * 4 + 2] = 0.0f;
        combined[i * 4 + 3] = 1.0f;
    }
    for (uint32_t sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex)
    {
        const float* sourceData = mapped + sourceIndex * pixelCount * 4;
        for (size_t i = 0; i < pixelCount; ++i)
        {
            combined[i * 4 + 0] += sourceData[i * 4 + 0];
            combined[i * 4 + 1] += sourceData[i * 4 + 1];
            combined[i * 4 + 2] += sourceData[i * 4 + 2];
        }
    }
    readbackBuf->Unmap();
    return combined;
}

static Vector<unsigned char> ReadbackRGBA8RT(RHITexture* sourceTexture, uint32_t& outWidth, uint32_t& outHeight)
{
    CHECK_MSG(sourceTexture->mDesc.format == RHIResourceFormat::R8G8B8A8Unorm,
              "SDR readback expects R8G8B8A8Unorm, got {}", sourceTexture->mDesc.format);
    uint32_t w = sourceTexture->mDesc.extent.x;
    uint32_t h = sourceTexture->mDesc.extent.y;
    outWidth = w;
    outHeight = h;

    const size_t pixelCount = static_cast<size_t>(w) * h;
    const size_t imageBytes = pixelCount * 4;
    auto readbackBuf = GContext->device->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Readback,
                                                                    .hostAccess = RHIResourceHostAccess::ReadWrite,
                                                                    .coherent = true},
                                                       .usage = RHIBufferUsageBits::TransferDestination,
                                                       .size = imageBytes});

    {
        ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
        auto* cmd = ctx.Get();
        cmd->Begin();
        cmd->BeginTransition();
        cmd->SetImageTransition(sourceTexture,
                                {.srcAccess = RHIResourceAccessBits::ShaderRead,
                                 .dstAccess = RHIResourceAccessBits::TransferRead,
                                 .srcStage = RHIPipelineStageBits::FragmentShader,
                                 .dstStage = RHIPipelineStageBits::Transfer,
                                 .srcImgLayout = RHITextureLayout::ShaderReadOnly,
                                 .dstImgLayout = RHITextureLayout::TransferSrc,
                                 .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->EndTransition();
        cmd->CopyImageToBuffer(
            sourceTexture, RHITextureLayout::TransferSrc, readbackBuf.Get(),
            {{{.dstBufferOffset = 0,
               .srcLayer = {.aspect = RHITextureAspectFlagBits::Color},
               .extent = {w, h, 1}}}});
        cmd->BeginTransition();
        cmd->SetImageTransition(sourceTexture,
                                {.srcAccess = RHIResourceAccessBits::TransferRead,
                                 .dstAccess = RHIResourceAccessBits::ShaderRead,
                                 .srcStage = RHIPipelineStageBits::Transfer,
                                 .dstStage = RHIPipelineStageBits::FragmentShader,
                                 .srcImgLayout = RHITextureLayout::TransferSrc,
                                 .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                                 .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->EndTransition();
        cmd->End();
        ctx.Submit();
        ctx.WaitIdle();
    }

    auto* mapped = readbackBuf->Map<unsigned char>();
    Vector<unsigned char> rgba(imageBytes, GLOBAL_ALLOC);
    std::memcpy(rgba.data(), mapped, imageBytes);
    readbackBuf->Unmap();
    return rgba;
}

void DoRenderReadback(RendererHandles const& handles)
{
    auto* renderer = GContext->renderer;
    uint32_t w = 0;
    uint32_t h = 0;

    if (GEditor.renderTask.format == ERenderFormat::HDR)
    {
        CHECK_MSG(handles.numHdrRT > 0u && handles.numHdrRT <= 2u, "Invalid HDR readback texture count");
        RHITexture* hdrTextures[2]{nullptr, nullptr};
        for (uint32_t i = 0; i < handles.numHdrRT; ++i)
            hdrTextures[i] = renderer->DerefResource(handles.hdrRT[i]).Get<RHITexture*>();
        Vector<float> combined = ReadbackAndCombineFloatRTs(hdrTextures, handles.numHdrRT, w, h);

        const char* hdrPath =
            GEditor.renderTask.outputPath.empty() ? "render_output.hdr" : GEditor.renderTask.outputPath.c_str();
        SaveHDR(combined.data(), static_cast<int>(w), static_cast<int>(h), hdrPath);
        LOG(Editor, LogInfo, "{} HDR image saved to {} ({}x{}, {} frames)",
            GEditor.rendererMode == ERendererMode::PathTracer ? "Path tracer" : "Raster", hdrPath, w, h,
            GEditor.shaderGlobals.ptAccumulatedFrames);
        return;
    }

    CHECK_MSG(handles.sdrRT != kInvalidHandle, "Invalid SDR readback texture");
    RHITexture* sdrTexture = renderer->DerefResource(handles.sdrRT).Get<RHITexture*>();
    Vector<unsigned char> sdr = ReadbackRGBA8RT(sdrTexture, w, h);
    const char* sdrPath =
        GEditor.renderTask.outputPath.empty() ? "render_output.png" : GEditor.renderTask.outputPath.c_str();
    SavePNG(sdr.data(), static_cast<int>(w), static_cast<int>(h), sdrPath);
    LOG(Editor, LogInfo, "{} SDR image saved to {} ({}x{}, {} frames)",
        GEditor.rendererMode == ERendererMode::PathTracer ? "Path tracer" : "Raster", sdrPath, w, h,
        GEditor.shaderGlobals.ptAccumulatedFrames);
}

void UpdateSceneLights()
{
    auto lights = GEditor.doc.Scene().GetLights();
    uint32_t count = static_cast<uint32_t>(lights.size());
    GEditor.shaderGlobals.numSceneLights = count;

    GEditor.doc.lights.clear();
    GEditor.doc.lights.resize(count);
    auto lightSamplerType = GContext->gpuScene
        ? GContext->gpuScene->mLightSamplerType
        : GPUScene::LightSamplerType::Power;
    for (uint32_t i = 0; i < count; i++)
    {
        auto& src = lights[i];
        FLightToGSLight(src, GEditor.doc.lights[i], lightSamplerType);
    }

    CommitSceneToGPU(false);
}

static uint32_t RemapTextureIndex(Vector<uint32_t> const& textureIDMap, uint32_t sourceIndex)
{
    return sourceIndex != kInvalidTexture ? textureIDMap[sourceIndex] : UINT32_MAX;
}

static void BuildEditorMaterials(Vector<uint32_t> const& textureIDMap)
{
    for (auto& src : GEditor.doc.Scene().GetMaterials())
    {
        auto& dst = GEditor.doc.materials.emplace_back();
        dst.baseColorFactor = src.baseColorFactor;
        dst.emissiveFactor = src.emissiveFactor;
        dst.metallicFactor = src.metallicFactor;
        dst.roughnessFactor = src.roughnessFactor;
        dst.baseColorTexture = RemapTextureIndex(textureIDMap, src.baseColorTexture);
        dst.emissiveTexture = RemapTextureIndex(textureIDMap, src.emissiveTexture);
        dst.metallicRoughnessTexture = RemapTextureIndex(textureIDMap, src.metallicRoughnessTexture);
        dst.normalTexture = RemapTextureIndex(textureIDMap, src.normalTexture);
        dst.transmissionTexture = RemapTextureIndex(textureIDMap, src.transmissionTexture);
        dst.specularTexture = RemapTextureIndex(textureIDMap, src.specularTexture);
        dst.specularColorTexture = RemapTextureIndex(textureIDMap, src.specularColorTexture);
        dst.anisotropyTexture = RemapTextureIndex(textureIDMap, src.anisotropyTexture);
        dst.transmissionFactor = src.transmissionFactor;
        dst.ior = src.ior;
        dst.specularFactor = src.specularFactor;
        dst.specularColorFactor = src.specularColorFactor;
        dst.anisotropyStrength = src.anisotropyStrength;
        dst.anisotropyRotation = src.anisotropyRotation;
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

static void BuildEditorInstances(Vector<uint32_t> const& meshOffsets, Vector<uint32_t> const& curveOffsets)
{
    auto instances = GEditor.doc.Scene().GetInstances();
    CHECK_MSG(instances.size() <= UINT32_MAX, "Too many scene instances");
    for (size_t i = 0; i < instances.size(); i++)
    {
        auto const& src = instances[i];
        if (src.type == FInstanceType::Mesh)
        {
            CHECK_MSG(src.resourceIndex < meshOffsets.size(), "Mesh instance references invalid mesh {}", src.resourceIndex);
            auto& dst = GEditor.doc.instances.emplace_back();
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
            auto& dst = GEditor.doc.instances.emplace_back();
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

static void ApplySceneCamera()
{
    auto cameras = GEditor.doc.Scene().GetCameras();
    if (cameras.empty())
        return;

    auto& camera = cameras.front();
    vec3 dir = camera.transform.rotation * vec3(0, 0, 1);
    GEditor.camera.center = camera.transform.transform - dir * GEditor.camera.radius;
    GEditor.camera.rot = camera.transform.rotation;
    GEditor.camera.fovY = camera.fovY;
    GEditor.aperture.dofEnabled = camera.lensEnabled;
    if (camera.lensEnabled)
    {
        GEditor.aperture.sensorHeightMm = camera.sensorHeightMm;
        GEditor.aperture.fStop = camera.fStop;
        GEditor.shaderGlobals.focalDistance = camera.focusDistance;
        GEditor.shaderGlobals.apertureBlades = camera.apertureBlades;
        GEditor.shaderGlobals.apertureRotation = camera.apertureRotation;
        GEditor.shaderGlobals.apertureRatio = camera.apertureRatio;
    }
}

static void ApplySceneEnvironment()
{
    auto const& environment = GEditor.doc.Scene().GetSceneGlobals();
    GEditor.shaderGlobals.ambientColor = environment.color;
    GEditor.shaderGlobals.ambientPower = environment.strength;
    GEditor.shaderGlobals.envAzimuthOffset = environment.azimuthOffset;
    GEditor.shaderGlobals.useEnvMap = 0u;
}

static size_t GetSceneUploadWorkerCount()
{
    return std::max<size_t>(1u, std::thread::hardware_concurrency());
}

static size_t GetSceneUploadTaskQueueSize(size_t taskCount)
{
    return ThreadPool::getTaskSize(std::max<size_t>(taskCount, 1u));
}

static GPUScene* RecreateGPUSceneForLoadedScene()
{
    const auto estimatedBudget = GPUScene::CalculateSceneBudget(GEditor.doc.Scene(), GContext->device->GetCapabilities());
    LOG(Editor, LogDebug,
        "Estimated GPUScene budget: primitive {} MB, curve AABB {} MB, instances {}, materials {}, lights {}, textures {}",
        estimatedBudget.primitiveBudget / (1u << 20),
        estimatedBudget.curveAABBBudget / (1u << 20),
        estimatedBudget.instanceBudget,
        estimatedBudget.materialBudget,
        estimatedBudget.lightBudget,
        estimatedBudget.texturesBudget);

    auto lightSamplerType = GPUScene::LightSamplerType::Power;
    if (GContext->gpuScene)
        lightSamplerType = GContext->gpuScene->mLightSamplerType;

    GContext->device->WaitIdle();
    DestroyEditorRenderer(GContext);
    if (GContext->gpuScene)
    {
        Destruct(GContext->allocator, GContext->gpuScene);
        GContext->gpuScene = nullptr;
    }

    auto* gpu = Construct<GPUScene>(GContext->allocator, GContext, estimatedBudget);
    gpu->mLightSamplerType = lightSamplerType;
    GContext->gpuScene = gpu;
    return gpu;
}

void ReplaceScene(StringView path)
{
    LOG(Editor, LogInfo, "Loading scene: {}", path);
    String scenePayloadPath;
    try
    {
        auto ext = std::filesystem::path(path.data()).extension().string();
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".fscn")
        {
            scenePayloadPath = String(path);
        }
        else
        {
            scenePayloadPath = (std::filesystem::path("last.fscn")).string();
            {
                MemoryMappedFile sceneFile(scenePayloadPath, 64ull * 1024ull * 1024ull);
                FScene writeScene(sceneFile);
                LoadScene(path, writeScene);
            }
        }
        GEditor.doc.OpenSceneFile(scenePayloadPath);
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to load scene: {}", path);
        return;
    }

    // Clear editor-side data
    GEditor.doc.instances.clear();
    GEditor.doc.materials.clear();
    GEditor.doc.meshes.clear();
    GEditor.doc.blases.clear();
    GEditor.doc.curves.clear();
    GEditor.doc.curveBlases.clear();
    GEditor.doc.lights.clear();
    
    GEditor.doc.selectedInstance = -1;
    GEditor.doc.selectedMaterial = -1;
    GEditor.doc.selectedLight = -1;
    auto const& sceneGlobals = GEditor.doc.Scene().GetSceneGlobals();
    GEditor.shaderGlobals.camEV = sceneGlobals.postExposure;
    GEditor.viewLUTSdrIndex = static_cast<int>(sceneGlobals.viewLutSdrIndex);
    GEditor.viewLUTHdrIndex = static_cast<int>(sceneGlobals.viewLutHdrIndex);
    GEditor.viewLUTSdrExternalPath.clear();
    GEditor.viewLUTHdrExternalPath.clear();
    ApplySceneEnvironment();

    const size_t sceneMeshCount = GEditor.doc.Scene().GetMeshes().size();
    const size_t sceneInstanceCount = GEditor.doc.Scene().GetInstances().size();
    const size_t sceneCurveCount = GEditor.doc.Scene().GetCurves().size();
    const size_t sceneMaterialCount = GEditor.doc.Scene().GetMaterials().size();
    auto* gpu = RecreateGPUSceneForLoadedScene();

    Vector<uint32_t> meshOffsets(GLOBAL_ALLOC);
    Vector<uint32_t> curveOffsets(GLOBAL_ALLOC);
    Vector<uint32_t> textureIDMap(GEditor.doc.Scene().GetTextures().size(), GLOBAL_ALLOC);

    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    // Upload meshes and textures
    {
        size_t stagedTaskCount = 0;
        stagedTaskCount += GEditor.doc.Scene().GetMeshes().size() * 7u;
        stagedTaskCount += GEditor.doc.Scene().GetCurves().size();
        for (auto const& srcDesc : GEditor.doc.Scene().GetTextures())
        {
            if (srcDesc.IsValid())
                stagedTaskCount += size_t(srcDesc.GetNumLayers()) * srcDesc.GetNumMips();
        }

        ThreadPool uploadPool(GetSceneUploadWorkerCount(), GetSceneUploadTaskQueueSize(stagedTaskCount),
                              GLOBAL_ALLOC, "SceneUpload");
        Vector<Future<void>> stagedFutures(GLOBAL_ALLOC);
        stagedFutures.reserve(stagedTaskCount);
        Vector<GPUScene::StagedUploadJob> stagedJobs(GLOBAL_ALLOC);

        auto ScheduleStagedJobs = [&]
        {
            for (GPUScene::StagedUploadJob const& job : stagedJobs)
            {
                stagedFutures.push_back(uploadPool.Push(
                    [job]
                    {
                        job.Write();
                    }));
            }
            stagedJobs.clear();
        };
        auto DrainStagedJobs = [&]
        {
            uploadPool.Join();
            for (Future<void>& future : stagedFutures)
                future.get();
            stagedFutures.clear();
        };

        ImmediateUpload upload(GContext->device.Get(), 512 * (1u << 20));
        upload.Begin();
        auto FlushUpload = [&]
        {
            DrainStagedJobs();
            upload.End(), upload.WaitIdle(), upload.Begin();
        };

        for (auto const& srcDesc : GEditor.doc.Scene().GetMeshes())
        {
            auto& dst = GEditor.doc.meshes.emplace_back();
            auto& offset = meshOffsets.emplace_back();
            stagedJobs.clear();
            if (!gpu->BeginUpload(&upload, GEditor.doc.Scene(), srcDesc, dst, offset, stagedJobs))
            {
                LOG(Editor, LogDebug, "Scene upload staging exhausted by mesh upload ({} bytes); flushing",
                    GPUScene::CalculateMeshPrimitiveSize(srcDesc));
                FlushUpload();
                CHECK_MSG(gpu->BeginUpload(&upload, GEditor.doc.Scene(), srcDesc, dst, offset, stagedJobs),
                          "Staging buffer too small for single mesh upload ({} bytes)",
                          GPUScene::CalculateMeshPrimitiveSize(srcDesc));
            }
            ScheduleStagedJobs();
        }
        for (auto const& srcDesc : GEditor.doc.Scene().GetCurves())
        {
            auto& dst = GEditor.doc.curves.emplace_back();
            auto& offset = curveOffsets.emplace_back();
            stagedJobs.clear();
            if (!gpu->BeginUpload(&upload, GEditor.doc.Scene(), srcDesc, dst, offset, stagedJobs))
            {
                LOG(Editor, LogDebug,
                    "Scene upload staging exhausted by curve upload ({} primitive bytes, {} AABB bytes); flushing",
                    GPUScene::CalculateCurvePrimitiveSize(srcDesc),
                    GPUScene::CalculateCurveAABBSize(srcDesc));
                FlushUpload();
                CHECK_MSG(gpu->BeginUpload(&upload, GEditor.doc.Scene(), srcDesc, dst, offset, stagedJobs),
                          "Staging buffer too small for single curve upload ({} primitive bytes, {} AABB bytes)",
                          GPUScene::CalculateCurvePrimitiveSize(srcDesc),
                          GPUScene::CalculateCurveAABBSize(srcDesc));
            }
            ScheduleStagedJobs();
        }
        for (int id = 0; auto const& srcDesc : GEditor.doc.Scene().GetTextures())
        {
            if (!srcDesc.IsValid())
            {
                textureIDMap[id] = UINT32_MAX;
            }
            else
            {
                stagedJobs.clear();
                if (!gpu->BeginUpload(&upload, GEditor.doc.Scene(), srcDesc, textureIDMap[id], stagedJobs))
                {
                    LOG(Editor, LogDebug, "Scene upload staging exhausted by texture {} upload ({} bytes); flushing",
                        id, srcDesc.data.decodedSize);
                    FlushUpload();
                    CHECK_MSG(gpu->BeginUpload(&upload, GEditor.doc.Scene(), srcDesc, textureIDMap[id], stagedJobs),
                              "Staging buffer too small for single texture upload (texture {}, {} bytes)",
                              id, srcDesc.data.decodedSize);
                }
                ScheduleStagedJobs();
            }
            id++;
        }
        DrainStagedJobs();
        upload.End(), upload.WaitIdle();
        FTexture sdr(GLOBAL_ALLOC);
        FTexture hdr(GLOBAL_ALLOC);
        upload.Begin();
        LoadSelectedViewLUTs(sdr, hdr);
        gpu->UploadViewLUTs(&upload, sdr, hdr);
        upload.End(), upload.WaitIdle();
    }

    BuildEditorMaterials(textureIDMap);
    BuildEditorInstances(meshOffsets, curveOffsets);

    // Apply camera and lighting data (needs to be done before UpdateGPUScene to have lights ready)
    {
        ApplySceneCamera();
        UpdateSceneLights();
    }

    // Build BLASes and rebuild TLAS
    {
        ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
        GEditor.doc.blases.resize(GEditor.doc.meshes.size());
        GEditor.doc.curveBlases.resize(GEditor.doc.curves.size());
        constexpr size_t kBLASBuildBatch = 32u;
        for (size_t i = 0; i < GEditor.doc.meshes.size(); i += kBLASBuildBatch)
        {
            Span<GSMesh> meshesBatch = GEditor.doc.meshes;
            Span<uint32_t> indicesBatch = GEditor.doc.blases;
            size_t batchSize = std::min(kBLASBuildBatch, GEditor.doc.meshes.size() - i);
            meshesBatch = meshesBatch.subspan(i, batchSize);
            indicesBatch = indicesBatch.subspan(i, batchSize);
            LOG(Editor, LogDebug, "Building BLAS {} to {}", i, i + batchSize);
            gpu->BuildBLAS(&ctx, meshesBatch, indicesBatch);
        }
        for (size_t i = 0; i < GEditor.doc.curves.size(); i += kBLASBuildBatch)
        {
            Span<GSCurveSet> curvesBatch = GEditor.doc.curves;
            Span<uint32_t> indicesBatch = GEditor.doc.curveBlases;
            size_t batchSize = std::min(kBLASBuildBatch, GEditor.doc.curves.size() - i);
            curvesBatch = curvesBatch.subspan(i, batchSize);
            indicesBatch = indicesBatch.subspan(i, batchSize);
            LOG(Editor, LogDebug, "Building curve BLAS {} to {}", i, i + batchSize);
            gpu->BuildCurveBLAS(&ctx, curvesBatch, indicesBatch);
        }
        LOG(Editor, LogDebug, "Rebuilding TLAS");
        ctx->Begin();
        gpu->BuildTLAS(ctx.Get(), GEditor.doc.instances, GEditor.doc.blases,
                       GEditor.doc.curveBlases, GEditor.doc.lights, false);
        ctx->End(), ctx.Submit(), ctx.WaitIdle();
    }

    LOG(Editor, LogInfo, "Scene load complete: {} meshes, {} instances, {} curves, {} materials",
        sceneMeshCount, sceneInstanceCount, sceneCurveCount, sceneMaterialCount);

    // Trigger renderer reconfiguration
    GEditor.state = FERunningEnter;
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
        ImmediateUpload upload(GContext->device.Get(), 256 * (1u << 20));
        upload.Begin();
        gpu->UploadEnvMap(&upload, tex);
        upload.End(), upload.WaitIdle();
        GEditor.doc.Scene().GetSceneGlobals() = {
            .type = FSceneEnvironmentType::EnvMap,
            .color = GEditor.shaderGlobals.ambientColor,
            .strength = GEditor.shaderGlobals.ambientPower,
            .azimuthOffset = GEditor.shaderGlobals.envAzimuthOffset,
        };
        GEditor.shaderGlobals.useEnvMap = 1u;
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
        // Renderer must be rebuilt to rebind the environment map resource
        GEditor.state = FERunningEnter;
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
        ReplaceScene(filePath);
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