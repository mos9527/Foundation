#include "EditorState.hpp"
#include "Scene/Mesh.hpp"
#include <Core/AllocatorStack.hpp>
#include <Core/Paths.hpp>
#include <Core/ThreadPool.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <exception>
#include <filesystem>
#include <numbers>
#include <utility>

static constexpr const char* kTempScenePath = "last.fscn";
// For non-ReBAR devices. Staging is required
// Also serves as a lower bound for the GPU buffer
static constexpr size_t kDefaultSceneUploadStagingBudget = 128ull * (1ull << 20);
static constexpr size_t kDefaultSceneLoadScratchBudget = 64ull * (1ull << 20);
// Accounts for alignment, etc...
static constexpr size_t kBudgetSlack = 4ull * (1ull << 20);

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

static GPUScene::UpdateResult CommitSceneToGPU(GPUScene* gpu, EditorDocument& doc, UBO& globals, bool resetAccumulation)
{
    CHECK(gpu);

    auto res = gpu->UpdateGPUScene(doc.instances, doc.materials, doc.lights);
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

    CommitSceneToGPU(GContext->gpuScene, GEditor.doc, GEditor.shaderGlobals, resetAccumulation);
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

// Max memory required for uploading the scene to the GPU
static size_t SceneStagingBufferBudget(FScene const& scene, bool directGeometryUpload, bool includeTextures)
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
    if (includeTextures)
        for (FSerializedTexture const& texture : scene.GetTextures())
        {
            if (texture.IsValid())
                budget = std::max(budget, static_cast<size_t>(texture.data.decodedSize) + kBudgetSlack);
        }
    return budget + kBudgetSlack;
}

static size_t SceneBlobScratchBudget(FScene const& scene)
{
    size_t budget = 0ull;
    auto IncludeBlob = [&](FBlobRef const& blob)
    {
        if (blob.codec == FBlobCodec::LZ4)
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
    return std::max<size_t>(AlignUp(budget + kBudgetSlack, alignof(std::max_align_t)), alignof(std::max_align_t));
}

static size_t EnvMapUploadStagingBudget(FTexture const& source)
{
    const size_t envMapBytes = static_cast<size_t>(source.GetSize());
    const size_t conditionalCDFBytes = static_cast<size_t>(source.GetWidth()) * source.GetHeight() * sizeof(float);
    const size_t marginalCDFBytes = static_cast<size_t>(source.GetHeight()) * sizeof(float);
    return envMapBytes + conditionalCDFBytes + marginalCDFBytes + kBudgetSlack;
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

static void BuildSceneLights(EditorDocument& doc, UBO& globals, GPUScene::LightSamplerType lightSamplerType)
{
    auto lights = doc.Scene().GetLights();
    CHECK_MSG(lights.size() <= UINT32_MAX, "Too many scene lights");
    uint32_t count = static_cast<uint32_t>(lights.size());
    globals.numSceneLights = count;

    doc.lights.clear();
    doc.lights.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        auto& src = lights[i];
        FLightToGSLight(src, doc.lights[i], lightSamplerType);
    }
}

void UpdateSceneLights()
{
    auto lightSamplerType = GContext->gpuScene
        ? GContext->gpuScene->mLightSamplerType
        : GPUScene::LightSamplerType::Power;
    BuildSceneLights(GEditor.doc, GEditor.shaderGlobals, lightSamplerType);

    CommitSceneToGPU(false);
}

static uint32_t RemapTextureIndex(Vector<uint32_t> const& textureIDMap, uint32_t sourceIndex)
{
    return sourceIndex != kInvalidTexture ? textureIDMap[sourceIndex] : UINT32_MAX;
}

static void BuildEditorMaterials(EditorDocument& doc, Vector<uint32_t> const& textureIDMap)
{
    for (auto& src : doc.Scene().GetMaterials())
    {
        auto& dst = doc.materials.emplace_back();
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

static void BuildEditorInstances(EditorDocument& doc, Vector<uint32_t> const& meshOffsets, Vector<uint32_t> const& curveOffsets)
{
    auto instances = doc.Scene().GetInstances();
    CHECK_MSG(instances.size() <= UINT32_MAX, "Too many scene instances");
    for (size_t i = 0; i < instances.size(); i++)
    {
        auto const& src = instances[i];
        if (src.type == FInstanceType::Mesh)
        {
            CHECK_MSG(src.resourceIndex < meshOffsets.size(), "Mesh instance references invalid mesh {}", src.resourceIndex);
            auto& dst = doc.instances.emplace_back();
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
            auto& dst = doc.instances.emplace_back();
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

static void ApplySceneCamera(EditorDocument const& doc, FArcballCamera& cameraState,
                             CameraApertureState& apertureState, UBO& globals)
{
    auto cameras = doc.Scene().GetCameras();
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


static void ApplySceneEnvironment(EditorDocument const& doc, UBO& globals)
{
    auto const& environment = doc.Scene().GetSceneGlobals();
    globals.ambientColor = environment.color;
    globals.ambientPower = environment.strength;
    globals.envAzimuthOffset = environment.azimuthOffset;
    globals.useEnvMap = 0u;
}

static size_t GetSceneUploadWorkerCount()
{
    return std::max<size_t>(1u, std::thread::hardware_concurrency());
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

static GPUScene* CreateGPUSceneForLoadedScene(EditorDocument const& doc, size_t& outBudgetBytes)
{
    const auto estimatedBudget = GPUScene::CalculateSceneBudget(doc.Scene(), GContext->device->GetCapabilities());
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
    Destruct(GContext->allocator, gpu);
    gpu = nullptr;
}

static double BuildAccelerationStructuresForScene(GPUScene* gpu, EditorDocument& doc)
{
    CHECK(gpu);

    auto const buildStart = std::chrono::steady_clock::now();
    ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
    doc.blases.resize(doc.meshes.size());
    doc.curveBlases.resize(doc.curves.size());
    // Drivers can parallelize multiple AS builds in one command buffer, but keep each submit bounded to avoid TDR.
    constexpr size_t kBLASBuildBatch = 32u;
    for (size_t i = 0; i < doc.meshes.size(); i += kBLASBuildBatch)
    {
        size_t batchSize = std::min(kBLASBuildBatch, doc.meshes.size() - i);
        Span<GSMesh> meshesBatch = doc.meshes;
        Span<uint32_t> indicesBatch = doc.blases;
        meshesBatch = meshesBatch.subspan(i, batchSize);
        indicesBatch = indicesBatch.subspan(i, batchSize);
        LOG(Editor, LogDebug, "Building BLAS {} to {}", i, i + batchSize);
        gpu->BuildBLAS(&ctx, meshesBatch, indicesBatch);
    }
    for (size_t i = 0; i < doc.curves.size(); i += kBLASBuildBatch)
    {
        size_t batchSize = std::min(kBLASBuildBatch, doc.curves.size() - i);
        Span<GSCurveSet> curvesBatch = doc.curves;
        Span<uint32_t> indicesBatch = doc.curveBlases;
        curvesBatch = curvesBatch.subspan(i, batchSize);
        indicesBatch = indicesBatch.subspan(i, batchSize);
        LOG(Editor, LogDebug, "Building curve BLAS {} to {}", i, i + batchSize);
        gpu->BuildCurveBLAS(&ctx, curvesBatch, indicesBatch);
    }
    LOG(Editor, LogDebug, "Rebuilding TLAS");
    auto* cmd = ctx.Get();
    cmd->Begin();
    auto tlasResult = gpu->BuildTLAS(cmd, doc.instances, doc.blases, doc.curveBlases, doc.lights, false);
    cmd->End();
    if (tlasResult == GPUScene::TLASBuildResult::Built)
        ctx.Submit(), ctx.WaitIdle();
    double const buildMs = MillisecondsSince(buildStart);
    LOG(Editor, LogInfo, "Scene acceleration structures built in {:.2f} ms ({} mesh BLAS, {} curve BLAS)",
        buildMs, doc.meshes.size(), doc.curves.size());
    return buildMs;
}

void ReplaceScene(StringView path)
{
    LOG(Editor, LogInfo, "Loading scene: {}", path);

    auto const loadStart = std::chrono::steady_clock::now();

    GPUScene* newGPUScene = nullptr;
    try
    {
        String scenePayloadPath;
        auto ext = std::filesystem::path(path.data()).extension().string();
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".fscn")
        {
            scenePayloadPath = String(path);
        }
        else
        {
            // GLTF import would commit to a file first too
            // In this way we don't need to have the whole scene allocated in memory
            // explicitly - and works the same way as loading from FSCN
            scenePayloadPath = (std::filesystem::path(kTempScenePath)).string();
            Allocator* importScratch = GLOBAL_ALLOC;
            MemoryMappedFile sceneFile(scenePayloadPath, 64ull * 1024ull * 1024ull /* grows on demand */);
            FScene writeScene(sceneFile, importScratch);
            LoadScene(path, writeScene, importScratch);
        }

        double const sceneBuildMs = MillisecondsSince(loadStart);
        // Scratch memory for scene loading
        // Do note that from here on out, it's all FSCN loading, where
        // the few places that intermediate allocation is required are:
        // - Offset maps
        // - Various small allocations
        // - Working allocator for ThreadPool
        ScopedArena sceneArena(GLOBAL_ALLOC, kDefaultSceneLoadScratchBudget);
        AllocatorStack sceneAlloc(sceneArena);
        size_t loadScratchPeak = 0;
        auto TrackLoadScratchPeak = [&]
        {
            size_t used = 0;
            size_t budget = 0;
            sceneAlloc.QueryBudget(used, budget);
            loadScratchPeak = std::max(loadScratchPeak, used);
        };

        EditorDocument newDoc;
        newDoc.OpenSceneFile(scenePayloadPath, &sceneAlloc);
        newDoc.selectedInstance = -1;
        newDoc.selectedMaterial = -1;
        newDoc.selectedLight = -1;

        UBO newGlobals = GEditor.shaderGlobals;
        FArcballCamera newCamera = GEditor.camera;
        CameraApertureState newAperture = GEditor.aperture;
        auto const& sceneGlobals = newDoc.Scene().GetSceneGlobals();
        newGlobals.camEV = sceneGlobals.postExposure;
        int newViewLUTSdrIndex = static_cast<int>(sceneGlobals.viewLutSdrIndex);
        int newViewLUTHdrIndex = static_cast<int>(sceneGlobals.viewLutHdrIndex);
        String newViewLUTSdrExternalPath;
        String newViewLUTHdrExternalPath;
        ApplySceneEnvironment(newDoc, newGlobals);

        const size_t sceneMeshCount = newDoc.Scene().GetMeshes().size();
        const size_t sceneInstanceCount = newDoc.Scene().GetInstances().size();
        const size_t sceneCurveCount = newDoc.Scene().GetCurves().size();
        const size_t sceneMaterialCount = newDoc.Scene().GetMaterials().size();
        size_t sceneGpuBudget = 0;
        newGPUScene = CreateGPUSceneForLoadedScene(newDoc, sceneGpuBudget);

        Vector<uint32_t> meshOffsets(&sceneAlloc);
        Vector<uint32_t> curveOffsets(&sceneAlloc);
        Vector<uint32_t> textureIDMap(newDoc.Scene().GetTextures().size(), &sceneAlloc);

        LOG(Editor, LogInfo, "Uploading new scene data to GPU");
        double uploadMs = 0.0;
        size_t blobBudget = 0;
        size_t blobLaneBudget = 0;
        size_t blobWorkerCount = 0;
        {
            auto const uploadStart = std::chrono::steady_clock::now();
            size_t stagedTaskCount = 0;
            stagedTaskCount += newDoc.Scene().GetMeshes().size() * 7u;
            stagedTaskCount += newDoc.Scene().GetCurves().size() * 4u;
            for (auto const& srcDesc : newDoc.Scene().GetTextures())
            {
                if (srcDesc.IsValid())
                    stagedTaskCount += size_t(srcDesc.GetNumLayers()) * srcDesc.GetNumMips();
            }
            // Scratch memory for blob decode. Each upload worker gets one lane, large enough for the largest decoded blob.
            const bool directGeometryUpload = newGPUScene->UsesDirectGeometryUpload();
            const size_t geometryResourceCount = sceneMeshCount + sceneCurveCount;
            blobWorkerCount = std::min(std::max<size_t>(1u, std::thread::hardware_concurrency()),
                                       std::max<size_t>(geometryResourceCount, 1u));
            blobLaneBudget = SceneBlobScratchBudget(newDoc.Scene());
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
            // Each worker exclusively owns one lane of the blob scratch memories
            struct SceneUploadJob : ThreadPoolJob
            {
                GPUScene::StagedUploadJob job;
                Span<Arena> scratchArenas;
                Span<AllocatorStack> scratchAllocators;
                SceneUploadJob(GPUScene::StagedUploadJob const& job, Span<Arena> scratchArenas,
                               Span<AllocatorStack> scratchAllocators) :
                    job(job), scratchArenas(scratchArenas), scratchAllocators(scratchAllocators)
                {
                }

                void Execute(size_t workerID) noexcept override
                {
                    if (job.kind == GPUScene::StagedUploadJob::Kind::Blob)
                    {
                        AllocatorStack& scratch = scratchAllocators[workerID];
                        scratch.Reset(scratchArenas[workerID]);
                        job.Write(&scratch);
                    }
                    else
                        job.Write();
                }
            };

            ThreadPool uploadPool(blobWorkerCount, GetSceneUploadTaskQueueSize(stagedTaskCount),
                                  &sceneAlloc, "SceneUpload");
            Vector<GPUScene::StagedUploadJob> stagedJobs(&sceneAlloc);
            auto ScheduleStagedJobs = [&]
            {
                for (GPUScene::StagedUploadJob const& job : stagedJobs)
                    uploadPool.PushImpl<SceneUploadJob>(job, blobScratchArenas, blobScratchAllocators);
                stagedJobs.clear();
            };
            auto FenceStagedJobs = [&]
            {
                uploadPool.Join();
                for (size_t i = 0; i < blobScratchAllocators.size(); ++i)
                    blobScratchAllocators[i].Reset(blobScratchArenas[i]);
            };
            const size_t uploadStaging = SceneStagingBufferBudget(newDoc.Scene(), directGeometryUpload, true);
            LOG(Editor, LogInfo, "Scene CPU Staging Budget: {} bytes", uploadStaging);
            ImmediateUpload upload(GContext->device.Get(), uploadStaging);
            upload.Begin();
            auto FlushUpload = [&]
            {
                FenceStagedJobs();
                upload.End(), upload.WaitIdle(), upload.Begin();
            };
            // Geometry Upload
            // See also SceneStagingBufferBudget
            for (auto const& srcDesc : newDoc.Scene().GetMeshes())
            {
                auto& dst = newDoc.meshes.emplace_back();
                auto& offset = meshOffsets.emplace_back();
                stagedJobs.clear();
                if (!newGPUScene->BeginUpload(&upload, newDoc.Scene(), srcDesc, dst, offset, stagedJobs))
                {
                    LOG(Editor, LogDebug, "Scene upload staging exhausted by mesh upload ({} bytes); flushing",
                        GPUScene::CalculateMeshPrimitiveSize(srcDesc));
                    FlushUpload();
                    stagedJobs.clear();
                    CHECK_MSG(newGPUScene->BeginUpload(&upload, newDoc.Scene(), srcDesc, dst, offset, stagedJobs),
                              "Staging buffer too small for single mesh upload ({} bytes)",
                              GPUScene::CalculateMeshPrimitiveSize(srcDesc));
                }
                ScheduleStagedJobs();
            }
            for (auto const& srcDesc : newDoc.Scene().GetCurves())
            {
                auto& dst = newDoc.curves.emplace_back();
                auto& offset = curveOffsets.emplace_back();
                stagedJobs.clear();
                if (!newGPUScene->BeginUpload(&upload, newDoc.Scene(), srcDesc, dst, offset, stagedJobs))
                {
                    LOG(Editor, LogDebug,
                        "Scene upload staging exhausted by curve upload ({} primitive bytes, {} AABB bytes); flushing",
                        GPUScene::CalculateCurvePrimitiveSize(srcDesc),
                        GPUScene::CalculateCurveAABBSize(srcDesc));
                    FlushUpload();
                    stagedJobs.clear();
                    CHECK_MSG(newGPUScene->BeginUpload(&upload, newDoc.Scene(), srcDesc, dst, offset, stagedJobs),
                              "Staging buffer too small for single curve upload ({} primitive bytes, {} AABB bytes)",
                              GPUScene::CalculateCurvePrimitiveSize(srcDesc),
                              GPUScene::CalculateCurveAABBSize(srcDesc));
                }
                ScheduleStagedJobs();
            }
            // Texture Upload
            // Always memcpy'd since these are block-compressed.
            for (int id = 0; auto const& srcDesc : newDoc.Scene().GetTextures())
            {
                if (!srcDesc.IsValid())
                {
                    textureIDMap[id] = UINT32_MAX;
                }
                else
                {
                    stagedJobs.clear();
                    if (!newGPUScene->BeginUpload(&upload, newDoc.Scene(), srcDesc, textureIDMap[id], stagedJobs))
                    {
                        LOG(Editor, LogDebug, "Scene upload staging exhausted by texture {} upload ({} bytes); flushing",
                            id, srcDesc.data.decodedSize);
                        FlushUpload();
                        stagedJobs.clear();
                        CHECK_MSG(newGPUScene->BeginUpload(&upload, newDoc.Scene(), srcDesc, textureIDMap[id], stagedJobs),
                                  "Staging buffer too small for single texture upload (texture {}, {} bytes)",
                                  id, srcDesc.data.decodedSize);
                    }
                    ScheduleStagedJobs();
                }
                id++;
            }
            FenceStagedJobs();
            if (newGPUScene->UsesDirectGeometryUpload())
                newGPUScene->FlushDirectGeometryUpload();
            upload.End(), upload.WaitIdle();
            FTexture sdr(&sceneAlloc);
            FTexture hdr(&sceneAlloc);
            upload.Begin();
            LoadSelectedViewLUTs(sdr, hdr, newViewLUTSdrIndex, newViewLUTHdrIndex,
                                 newViewLUTSdrExternalPath, newViewLUTHdrExternalPath, &sceneAlloc);
            newGPUScene->UploadViewLUTs(&upload, sdr, hdr);
            upload.End(), upload.WaitIdle();
            uploadMs = MillisecondsSince(uploadStart);
        }
        LOG(Editor, LogInfo, "Scene GPU upload complete in {:.2f} ms (blob scratch {:.2f} MB, total {:.2f} MB across {} workers)",
            uploadMs, double(blobLaneBudget) / double(1u << 20),
            double(blobBudget) / double(1u << 20), blobWorkerCount);

        BuildEditorMaterials(newDoc, textureIDMap);
        BuildEditorInstances(newDoc, meshOffsets, curveOffsets);
        ApplySceneCamera(newDoc, newCamera, newAperture, newGlobals);
        BuildSceneLights(newDoc, newGlobals, newGPUScene->mLightSamplerType);
        CommitSceneToGPU(newGPUScene, newDoc, newGlobals, true);
        newGlobals.ggxLutEIndex = newGPUScene->GetGGXLutEIndex();
        newGlobals.viewLutIndex = GContext->enableHDR
            ? newGPUScene->GetViewLutHdrIndex()
            : newGPUScene->GetViewLutSdrIndex();
        newGlobals.envMapTextureIndex = newGPUScene->GetEnvMapIndexOrDefault();
        newGlobals.envMapMarginalCDFIndex = newGPUScene->GetEnvMapMarginalCDFIndexOrDefault();
        newGlobals.envMapConditionalCDFIndex = newGPUScene->GetEnvMapConditionalCDFIndexOrDefault();
        double const blasMs = BuildAccelerationStructuresForScene(newGPUScene, newDoc);

        EditorDocument commitDoc;
        commitDoc.OpenSceneFile(scenePayloadPath, &sceneAlloc);
        commitDoc.instances = std::move(newDoc.instances);
        commitDoc.materials = std::move(newDoc.materials);
        commitDoc.meshes = std::move(newDoc.meshes);
        commitDoc.blases = std::move(newDoc.blases);
        commitDoc.curves = std::move(newDoc.curves);
        commitDoc.curveBlases = std::move(newDoc.curveBlases);
        commitDoc.lights = std::move(newDoc.lights);
        commitDoc.selectedInstance = newDoc.selectedInstance;
        commitDoc.selectedMaterial = newDoc.selectedMaterial;
        commitDoc.selectedLight = newDoc.selectedLight;

        GContext->device->WaitIdle();
        DestroyEditorRenderer(GContext);
        DestroyGPUScene(GContext->gpuScene);
        GContext->gpuScene = newGPUScene;
        newGPUScene = nullptr;
        GEditor.doc.OpenSceneFile(scenePayloadPath);
        GEditor.doc.instances = std::move(commitDoc.instances);
        GEditor.doc.materials = std::move(commitDoc.materials);
        GEditor.doc.meshes = std::move(commitDoc.meshes);
        GEditor.doc.blases = std::move(commitDoc.blases);
        GEditor.doc.curves = std::move(commitDoc.curves);
        GEditor.doc.curveBlases = std::move(commitDoc.curveBlases);
        GEditor.doc.lights = std::move(commitDoc.lights);
        GEditor.doc.selectedInstance = commitDoc.selectedInstance;
        GEditor.doc.selectedMaterial = commitDoc.selectedMaterial;
        GEditor.doc.selectedLight = commitDoc.selectedLight;
        GEditor.shaderGlobals = newGlobals;
        GEditor.camera = newCamera;
        GEditor.aperture = newAperture;
        GEditor.viewLUTSdrIndex = newViewLUTSdrIndex;
        GEditor.viewLUTHdrIndex = newViewLUTHdrIndex;
        GEditor.viewLUTSdrExternalPath = std::move(newViewLUTSdrExternalPath);
        GEditor.viewLUTHdrExternalPath = std::move(newViewLUTHdrExternalPath);
        GEditor.cameraUpdated = true;
        GEditor.state = FERunningEnter;

        TrackLoadScratchPeak();
        size_t currentLoadScratchUsed = 0;
        size_t loadScratchBudget = 0;
        sceneAlloc.QueryBudget(currentLoadScratchUsed, loadScratchBudget);
        double const loadMs = MillisecondsSince(loadStart);
        double const gpuRateMbS = loadMs > 0.0 ?
            (double(sceneGpuBudget) / double(1u << 20)) / (loadMs * 1e-3) : 0.0;
        LOG(Editor, LogInfo, "Scene load complete in {:.2f} ms (build {:.2f} ms, upload {:.2f} ms, BLAS {:.2f} ms, avg GPU rate {:.2f} MB/s, load scratch peak {:.2f} / {:.2f} MB): {} meshes, {} instances, {} curves, {} materials",
            loadMs, sceneBuildMs, uploadMs, blasMs, gpuRateMbS,
            double(loadScratchPeak) / double(1u << 20), double(loadScratchBudget) / double(1u << 20),
            sceneMeshCount, sceneInstanceCount, sceneCurveCount, sceneMaterialCount);
    }
    catch (std::exception const& e)
    {
        DestroyGPUScene(newGPUScene);
        LOG(Editor, LogError, "Failed to load scene: {} ({})", path, e.what());
    }
    catch (...)
    {
        DestroyGPUScene(newGPUScene);
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
        GEditor.doc.Scene().GetSceneGlobals() = {
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