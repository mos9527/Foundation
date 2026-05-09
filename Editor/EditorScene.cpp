#include "EditorState.hpp"
#include "Scene/Mesh.hpp"
#include <Core/Paths.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include <algorithm>
#include <filesystem>
#include <numbers>

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
    auto res = GContext->gpuScene->UpdateGPUScene(GEditor.doc.instances, GEditor.doc.curveInstances,
                                                  GEditor.doc.materials, GEditor.doc.lights);
    GEditor.shaderGlobals.firstInstance = res.firstInstance;
    GEditor.shaderGlobals.numInstances  = res.numInstances;
    GEditor.shaderGlobals.firstCurveInstance = res.firstCurveInstance;
    GEditor.shaderGlobals.numCurveInstances  = res.numCurveInstances;
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

void ApplyViewLUTSelection()
{
    GEditor.viewLUTSdrIndex = std::clamp(GEditor.viewLUTSdrIndex, 0, kViewLUTSdrCount - 1);
    GEditor.viewLUTHdrIndex = std::clamp(GEditor.viewLUTHdrIndex, 0, kViewLUTHdrCount - 1);

    FTexture sdr = LoadViewLUT(kViewLUTsSdr[GEditor.viewLUTSdrIndex].path);
    FTexture hdr = LoadViewLUT(kViewLUTsHdr[GEditor.viewLUTHdrIndex].path);

    ImmediateUpload upload(GContext->device.Get(), sdr.GetSize() + hdr.GetSize());
    upload.Begin();
    GContext->gpuScene->UploadViewLUTs(&upload, sdr, hdr);
    upload.End();
    upload.WaitIdle();

    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
    GEditor.state = FERunningEnter;
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

static void SaveSDRPNG(float const* data, uint32_t width, uint32_t height, StringView path)
{
    Vector<unsigned char> rgba(static_cast<size_t>(width) * height * 4, GLOBAL_ALLOC);
    const size_t pixelCount = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixelCount; ++i)
    {
        rgba[i * 4 + 0] = static_cast<unsigned char>(std::clamp(data[i * 4 + 0], 0.0f, 1.0f) * 255.0f + 0.5f);
        rgba[i * 4 + 1] = static_cast<unsigned char>(std::clamp(data[i * 4 + 1], 0.0f, 1.0f) * 255.0f + 0.5f);
        rgba[i * 4 + 2] = static_cast<unsigned char>(std::clamp(data[i * 4 + 2], 0.0f, 1.0f) * 255.0f + 0.5f);
        rgba[i * 4 + 3] = static_cast<unsigned char>(std::clamp(data[i * 4 + 3], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    SavePNG(rgba.data(), static_cast<int>(width), static_cast<int>(height), path);
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
    Vector<float> sdr = ReadbackAndCombineFloatRTs(&sdrTexture, 1u, w, h);
    const char* sdrPath =
        GEditor.renderTask.outputPath.empty() ? "render_output.png" : GEditor.renderTask.outputPath.c_str();
    SaveSDRPNG(sdr.data(), w, h, sdrPath);
    LOG(Editor, LogInfo, "{} SDR image saved to {} ({}x{}, {} frames)",
        GEditor.rendererMode == ERendererMode::PathTracer ? "Path tracer" : "Raster", sdrPath, w, h,
        GEditor.shaderGlobals.ptAccumulatedFrames);
}

void UpdateSceneLights()
{
    uint32_t count = static_cast<uint32_t>(GEditor.doc.scene.mLights.size());
    GEditor.shaderGlobals.numSceneLights = count;

    GEditor.doc.lights.clear();
    GEditor.doc.lights.resize(count);
    for (uint32_t i = 0; i < count; i++)
    {
        auto& src = GEditor.doc.scene.mLights[i];
        FLightToGSLight(src, GEditor.doc.lights[i], GContext->gpuScene->mLightSamplerType);
    }

    CommitSceneToGPU(false);
}

static uint32_t RemapTextureIndex(Vector<uint32_t> const& textureIDMap, uint32_t sourceIndex)
{
    return sourceIndex != kInvalidTexture ? textureIDMap[sourceIndex] : UINT32_MAX;
}

static void BuildEditorMaterials(Vector<uint32_t> const& textureIDMap)
{
    for (auto& src : GEditor.doc.scene.mMaterials)
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

static void BuildEditorInstances(Vector<uint32_t> const& meshOffsets)
{
    for (auto& src : GEditor.doc.scene.mInstances)
    {
        auto& dst = GEditor.doc.instances.emplace_back();
        dst.transform = src.transform.transform;
        dst.rotation = src.transform.rotation;
        dst.scale = src.transform.scale;
        dst.meshOffset = meshOffsets[src.meshIndex];
        dst.materialIndex = src.materialIndex;
        dst.meshIndex = src.meshIndex;
    }
}

static void BuildEditorCurveInstances(Vector<uint32_t> const& curveOffsets)
{
    for (auto& src : GEditor.doc.scene.mCurveInstances)
    {
        auto& dst = GEditor.doc.curveInstances.emplace_back();
        dst.transform = src.transform.transform;
        dst.rotation = src.transform.rotation;
        dst.scale = src.transform.scale;
        dst.curveOffset = curveOffsets[src.curveIndex];
        dst.materialIndex = src.materialIndex;
        dst.curveIndex = src.curveIndex;
    }
}

static void ApplySceneCamera()
{
    if (GEditor.doc.scene.mCameras.empty())
        return;

    auto& camera = GEditor.doc.scene.mCameras.front();
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

void ReplaceScene(StringView path)
{
    LOG(Editor, LogInfo, "Loading scene: {}", path);
    GEditor.doc.scene = FScene(GLOBAL_ALLOC);
    try
    {
        LoadScene(path, GEditor.doc.scene);
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
    GEditor.doc.curveInstances.clear();
    GEditor.doc.curves.clear();
    GEditor.doc.curveBlases.clear();
    GEditor.doc.lights.clear();
    
    GEditor.doc.selectedInstance = -1;
    GEditor.doc.selectedCurveInstance = -1;
    GEditor.doc.selectedMaterial = -1;
    GEditor.doc.selectedLight = -1;

    auto* gpu = GContext->gpuScene;
    gpu->Reset();

    Vector<uint32_t> meshOffsets(GLOBAL_ALLOC);
    Vector<uint32_t> curveOffsets(GLOBAL_ALLOC);
    Vector<uint32_t> textureIDMap(GEditor.doc.scene.mTextures.size(), GLOBAL_ALLOC);

    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    // Upload meshes and textures
    {
        ImmediateUpload upload(GContext->device.Get(), 512 * (1u << 20));
        upload.Begin();
        for (auto& src : GEditor.doc.scene.mMeshes)
        {
            CHECK(src.EnsureQuantized());
            CHECK(src.EnsureRaw());
            auto& dst = GEditor.doc.meshes.emplace_back();
            auto& offset = meshOffsets.emplace_back();
            if (!gpu->Upload(&upload, src, dst, offset))
            {
                upload.End(), upload.WaitIdle(), upload.Begin();
                CHECK_MSG(gpu->Upload(&upload, src, dst, offset), "Staging buffer too small for single mesh upload");
            }
        }
        for (auto& src : GEditor.doc.scene.mCurves)
        {
            auto& dst = GEditor.doc.curves.emplace_back();
            auto& offset = curveOffsets.emplace_back();
            if (!gpu->Upload(&upload, src, dst, offset))
            {
                upload.End(), upload.WaitIdle(), upload.Begin();
                CHECK_MSG(gpu->Upload(&upload, src, dst, offset), "Staging buffer too small for single curve upload");
            }
        }
        for (int id = 0; auto& src : GEditor.doc.scene.mTextures)
        {
            if (!src.IsValid())
            {
                textureIDMap[id] = UINT32_MAX;
            }
            else
            {
                if (!gpu->Upload(&upload, src, textureIDMap[id]))
                {
                    upload.End(), upload.WaitIdle(), upload.Begin();
                    CHECK_MSG(gpu->Upload(&upload, src, textureIDMap[id]),
                              "Staging buffer too small for single texture upload");
                }
            }
            id++;
        }
        const bool hasCustomViewLUT = GEditor.doc.scene.mViewLutSdr.IsValid() || GEditor.doc.scene.mViewLutHdr.IsValid();
        if (hasCustomViewLUT)
        {
            CHECK_MSG(GEditor.doc.scene.mViewLutSdr.IsValid() && GEditor.doc.scene.mViewLutHdr.IsValid(),
                      "Scene view LUT override must provide both SDR and HDR LUTs");
            gpu->UploadViewLUTs(&upload, GEditor.doc.scene.mViewLutSdr, GEditor.doc.scene.mViewLutHdr);
        }
        upload.End(), upload.WaitIdle();
    }

    BuildEditorMaterials(textureIDMap);
    BuildEditorInstances(meshOffsets);
    BuildEditorCurveInstances(curveOffsets);

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
                       GEditor.doc.curveInstances, GEditor.doc.curveBlases, GEditor.doc.lights, false);
        ctx->End(), ctx.Submit(), ctx.WaitIdle();
    }

    LOG(Editor, LogInfo, "Scene load complete: {} meshes, {} mesh instances, {} curves, {} curve instances, {} materials",
        GEditor.doc.scene.mMeshes.size(), GEditor.doc.scene.mInstances.size(),
        GEditor.doc.scene.mCurves.size(), GEditor.doc.scene.mCurveInstances.size(), GEditor.doc.scene.mMaterials.size());

    // Trigger renderer reconfiguration
    GEditor.state = FERunningEnter;
}

void LoadEnvMap(StringView path)
{
    LOG(Editor, LogInfo, "Loading HDRI env map: {}", path);
    auto* gpu = GContext->gpuScene;
    try
    {
        FTexture tex(GLOBAL_ALLOC);
        LoadHDR(tex, path);
        ImmediateUpload upload(GContext->device.Get(), 256 * (1u << 20));
        upload.Begin();
        gpu->UploadEnvMap(&upload, tex);
        upload.End(), upload.WaitIdle();
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

void SaveScene(StringView path)
{
    LOG(Editor, LogInfo, "Saving scene to: {}", path);
    try
    {
        FileWriter writer(path);
        FSerialize(writer, GEditor.doc.scene);
        GEditor.doc.currentSavePath = String(path);
        LOG(Editor, LogInfo, "Scene saved successfully");
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to save scene to: {}", path);
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
        else if (std::filesystem::exists(hdriPath + ".exr"))
            LoadEnvMap(hdriPath + ".exr");
    }
    else if (ext == ".hdr" || ext == ".hdri" || ext == ".exr")
    {
        LoadEnvMap(filePath);
    }
    else
    {
        LOG(Editor, LogWarn, "Unknown file type dropped: '{}'", filePath);
    }
}