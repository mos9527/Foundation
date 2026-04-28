#include "EditorState.hpp"
#include "Scene/Mesh.hpp"
#include <RenderCore/ImmediateContext.hpp>
#include <filesystem>
#include <numbers>

void FLightToGSLight(FLight const& src, GSLight& dst, GPUScene::LightSamplerType sampler)
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
            dst.power = src.power / totalArea;
        }
    }
    // Selection weight
    float weight = 1.0f;
    if (sampler == GPUScene::LightSamplerType::Power) {
        float luminance = 0.2126f * dst.color.x + 0.7152f * dst.color.y + 0.0722f * dst.color.z;
        weight = luminance * dst.power;
        if (dst.type == 3)
            weight *= dst.radius.x * dst.radius.y * pi<float>() * (dst.twoSided != 0 ? 2.0f : 1.0f);
        else if (dst.type == 4)
            weight *= cross(dst.dpdu, dst.dpdv).length() * 4.0f * (dst.twoSided != 0 ? 2.0f : 1.0f);
    }
    dst.selectionWeight = std::max(0.0f, weight);
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

    // Update GPU scene with lights
    auto* gpu = GContext->gpuScene;
    auto res = gpu->UpdateGPUScene(GEditor.doc.instances, GEditor.doc.materials, GEditor.doc.lights);
    GEditor.shaderGlobals.firstInstance = res.firstInstance;
    GEditor.shaderGlobals.numInstances  = res.numInstances;
    GEditor.shaderGlobals.firstMaterial = res.firstMaterial;
    GEditor.shaderGlobals.numMaterials  = res.numMaterials;
    GEditor.shaderGlobals.firstLight    = res.firstLight;
    GEditor.shaderGlobals.firstLightAliasTable = res.firstLightAliasTable;
    GEditor.shaderGlobals.sceneLightWeightSum = res.sceneLightWeightSum;
}

/* ==================== ReplaceScene ==================== */
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
    GEditor.doc.lights.clear();
    
    GEditor.doc.selectedInstance = -1;
    GEditor.doc.selectedMaterial = -1;
    GEditor.doc.selectedLight = -1;

    auto* gpu = GContext->gpuScene;
    gpu->Reset();

    Vector<uint32_t> meshOffsets(GLOBAL_ALLOC);
    Vector<uint32_t> textureIDMap(GEditor.doc.scene.mTextures.size(), GLOBAL_ALLOC);

    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    // Upload meshes and textures
    {
        ImmediateUpload upload(GContext->device.Get(), 128 * (1u << 20));
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
        upload.End(), upload.WaitIdle();
    }

    // Materials: remap texture indices
    for (auto& src : GEditor.doc.scene.mMaterials)
    {
        auto& dst = GEditor.doc.materials.emplace_back();
        dst.baseColorFactor = src.baseColorFactor;
        dst.emissiveFactor = src.emissiveFactor;
        dst.metallicFactor = src.metallicFactor;
        dst.roughnessFactor = src.roughnessFactor;
        dst.baseColorTexture = src.baseColorTexture != kInvalidTexture ? textureIDMap[src.baseColorTexture] : UINT32_MAX;
        dst.emissiveTexture = src.emissiveTexture != kInvalidTexture ? textureIDMap[src.emissiveTexture] : UINT32_MAX;
        dst.metallicRoughnessTexture = src.metallicRoughnessTexture != kInvalidTexture ? textureIDMap[src.metallicRoughnessTexture] : UINT32_MAX;
        dst.normalTexture = src.normalTexture != kInvalidTexture ? textureIDMap[src.normalTexture] : UINT32_MAX;
        dst.transmissionFactor = src.transmissionFactor;
        dst.ior = src.ior;
        dst.subsurfaceFactor = src.subsurfaceFactor;
        dst.subsurfaceColor = src.subsurfaceColor;
        dst.subsurfaceRadius = src.subsurfaceRadius;
    }

    // Instances
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

    // Apply camera and lighting data (needs to be done before UpdateGPUScene to have lights ready)
    {
        if (!GEditor.doc.scene.mCameras.empty())
        {
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
        UpdateSceneLights();
    }

    // Build BLASes and rebuild TLAS
    {
        ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
        GEditor.doc.blases.resize(GEditor.doc.meshes.size());
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
        LOG(Editor, LogDebug, "Rebuilding TLAS");
        ctx->Begin();
        gpu->BuildTLAS(ctx.Get(), GEditor.doc.instances, GEditor.doc.blases, GEditor.doc.lights, false);
        ctx->End(), ctx.Submit(), ctx.WaitIdle();
    }

    LOG(Editor, LogInfo, "Scene load complete: {} meshes, {} instances, {} materials",
        GEditor.doc.scene.mMeshes.size(), GEditor.doc.scene.mInstances.size(), GEditor.doc.scene.mMaterials.size());

    // Trigger renderer reconfiguration
    GEditor.state = FERunningEnter;
}

/* ==================== LoadEnvMap ==================== */
void LoadEnvMap(StringView path)
{
    LOG(Editor, LogInfo, "Loading HDRI env map: {}", path);
    auto* gpu = GContext->gpuScene;
    try
    {
        FTexture2D tex(GLOBAL_ALLOC);
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

/* ==================== SaveScene ==================== */
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

/* -- Drag-and-drop file handler: dispatch to the appropriate loader based on file extension -- */
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