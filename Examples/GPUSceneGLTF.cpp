// Loads a glTF/GLB/FSCN through the editor scene importer and views it with GPUScene.
#include <Renderer/GPUScene.hpp>
#include <Renderer/Renderer.hpp>
#include <Editor/Scene/Scene.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <Core/Paths.hpp>
#include "Examples.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <filesystem>
#include <numbers>

using namespace RenderUtils;
using Foundation::Core::PathsResolve;

namespace
{
static constexpr const char* kTempScenePath = "Cache/GPUSceneGLTF.fscn";

String LowerExtension(StringView path)
{
    String ext = std::filesystem::path(path.data()).extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

String PrepareScenePayload(StringView path)
{
    if (LowerExtension(path) == ".fscn")
        return String(path);

    String scenePayloadPath = PathsResolve(kTempScenePath);
    std::filesystem::path scenePayloadDir = std::filesystem::path(scenePayloadPath).parent_path();
    if (!scenePayloadDir.empty())
        std::filesystem::create_directories(scenePayloadDir);

    {
        MemoryMappedFile sceneFile(scenePayloadPath, 256ull * 1024ull * 1024ull /* grows on demand */);
        FImportedScene writeScene(sceneFile, GLOBAL_ALLOC);
        LoadScene(path, writeScene, GLOBAL_ALLOC);
    } // FImportedScene finalizes the cache file here.
    return scenePayloadPath;
}

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
                      "Serialized texture subresource size mismatch: layer {}, mip {}, blob {}, expected {}",
                      layer, mip, blob.decodedSize, dst.size_bytes());
            CHECK(scene.ReadBlob(blob, dst.data(), dst.size_bytes(), alloc));
        }
    }
    return texture;
}

bool IsSceneEnvironmentTexture(FImportedScene const& scene, size_t textureIndex)
{
    FLight const* environment = scene.GetEnvironmentLight();
    return environment != nullptr &&
        environment->environmentMap &&
        environment->environmentTexture != kInvalidTexture &&
        textureIndex == environment->environmentTexture;
}

void FillGSMaterial(GSMaterial& dst, FMaterial const& src, Vector<TextureHandle> const& textureIDMap, GPUScene* gpu)
{
    auto RemapTextureIndex = [&](uint32_t index) -> uint32_t
    {
        if (index == UINT32_MAX || index >= textureIDMap.size())
            return UINT32_MAX;
        TextureHandle const handle = textureIDMap[index];
        if (!handle.IsValid() || gpu->Query(handle) != GPUScene::Result::Ready)
            return UINT32_MAX;
        return handle.index;
    };

    dst.baseColorFactor = src.baseColorFactor;
    dst.emissiveFactor = float3(src.emissiveFactor) * src.emissiveFactor.w;
    dst.metallicFactor = src.metallicFactor;
    dst.roughnessFactor = src.roughnessFactor;
    dst.baseColorTexture = RemapTextureIndex(src.baseColorTexture);
    dst.emissiveTexture = RemapTextureIndex(src.emissiveTexture);
    dst.metallicRoughnessTexture = RemapTextureIndex(src.metallicRoughnessTexture);
    dst.normalTexture = RemapTextureIndex(src.normalTexture);
    dst.normalScale = src.normalScale;
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

float LightColorImportance(float3 color)
{
    return std::max(0.0f, std::max(color.x, std::max(color.y, color.z)));
}

float SpotLightImportanceSolidAngle(float cosInner, float cosOuter)
{
    float cosFalloffStart = std::clamp(std::max(cosInner, cosOuter), -1.0f, 1.0f);
    float cosTotalWidth = std::clamp(std::min(cosInner, cosOuter), -1.0f, cosFalloffStart);
    return 2.0f * std::numbers::pi_v<float> *
           ((1.0f - cosFalloffStart) + (cosFalloffStart - cosTotalWidth) / 5.0f);
}

float AreaLightFluxImportance(GSLight const& light)
{
    float area = 1.0f;
    if (light.type == static_cast<uint32_t>(FLightType::Disk))
        area = std::numbers::pi_v<float> * light.radius.x * light.radius.y;
    else if (light.type == static_cast<uint32_t>(FLightType::Rect))
        area = 4.0f * cross(light.dpdu, light.dpdv).length();

    float sides = light.twoSided != 0 ? 2.0f : 1.0f;
    return light.power * area * std::numbers::pi_v<float> * sides;
}

void FLightToGSLight(FLight const& src, GSLight& dst, GPUScene const* gpu, GPUScene::LightSamplerType sampler)
{
    dst = GSLight{};
    dst.type = static_cast<uint32_t>(src.type);
    dst.color = src.color;
    dst.power = src.power;
    dst.range = src.range;
    dst.position = src.transform.transform;
    dst.direction = normalize(src.transform.rotation * float3(0, 0, -1));
    dst.angularDiameter = src.angularDiameter;
    dst.spotInnerCosAngle = std::cos(src.spotInnerConeAngle);
    dst.spotOuterCosAngle = std::cos(src.spotOuterConeAngle);
    float areaWidth = std::max(src.width, 1e-6f);
    float areaHeight = std::max(src.height, 1e-6f);
    dst.radius = float2(areaWidth, areaHeight);
    dst.twoSided = src.twoSided ? 1u : 0u;
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
        bool const hasEnvMap = src.environmentMap && gpu && gpu->HasEnvMap();
        dst.color = hasEnvMap ? float3(1.0f) : src.color;
        dst.power = src.power;
        dst.twoSided = hasEnvMap ? 1u : 0u;
        dst.envAzimuthOffset = src.environmentAzimuthOffset;
    }

    float importance = 1.0f;
    if (sampler == GPUScene::LightSamplerType::Importance)
    {
        constexpr float kEnvDirectionalImportance = 10.0f;
        switch (src.type)
        {
        case FLightType::Environment:
            importance = LightColorImportance(dst.color) * dst.power * kEnvDirectionalImportance;
            break;
        case FLightType::Directional:
            importance = dst.power * kEnvDirectionalImportance;
            break;
        case FLightType::Point:
            importance = dst.power * 4.0f * std::numbers::pi_v<float>;
            break;
        case FLightType::Spot:
            importance = dst.power * SpotLightImportanceSolidAngle(dst.spotInnerCosAngle, dst.spotOuterCosAngle);
            break;
        case FLightType::Disk:
        case FLightType::Rect:
            importance = AreaLightFluxImportance(dst);
            break;
        }
        importance *= LightColorImportance(dst.color);
    }
    dst.importance = std::max(0.0f, importance);
}

FSerializedBounds TransformedBounds(FSerializedBounds const& local, FTransform const& transform)
{
    FSerializedBounds bounds = FSerializedBounds::Empty();
    if (!local.IsValid())
        return bounds;

    for (uint32_t i = 0; i < 8; ++i)
    {
        float3 p{
            (i & 1u) ? local.max.x : local.min.x,
            (i & 2u) ? local.max.y : local.min.y,
            (i & 4u) ? local.max.z : local.min.z};
        p = transform.transform + (transform.rotation * p) * transform.scale;
        bounds += p;
    }
    return bounds;
}

FSerializedBounds CalculateSceneBounds(FImportedScene const& scene)
{
    FSerializedBounds bounds = FSerializedBounds::Empty();
    Span<FSerializedMesh const> meshes = scene.GetMeshes();
    for (FInstance const& instance : scene.GetInstances())
    {
        if (instance.type != FInstanceType::Mesh || instance.resourceIndex >= meshes.size())
            continue;
        bounds += TransformedBounds(meshes[instance.resourceIndex].bounds, instance.transform);
    }
    return bounds;
}

void ApplySceneCamera(FImportedScene const& scene, FExampleOrbitCamera& camera)
{
    auto cameras = scene.GetCameras();
    if (!cameras.empty())
    {
        FCamera const& src = cameras.front();
        float3 dir = src.transform.rotation * float3(0, 0, 1);
        camera.rot = src.transform.rotation;
        camera.center = src.transform.transform - dir;
        camera.radius = 1.0f;
        camera.fovY = src.fovY;
        camera.zNear = 0.01f;
        return;
    }

    FSerializedBounds bounds = CalculateSceneBounds(scene);
    if (bounds.IsValid())
    {
        camera.center = (bounds.min + bounds.max) * 0.5f;
        float3 extent = bounds.max - bounds.min;
        camera.radius = std::max(1.0f, extent.length() * 0.9f);
    }
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("Usage: %s <scene.gltf|scene.glb|scene.fscn>\n", argv[0]);
        return 1;
    }

    String scenePath = argv[1];
    String scenePayloadPath = PrepareScenePayload(scenePath);
    MemoryMappedFile sceneFile(scenePayloadPath, MemoryMappedAccess::ReadOnly);
    FImportedScene scene(sceneFile, GLOBAL_ALLOC);
    LoadFSCN(scene);

    SDL_Window* window = SDL_CreateWindow("GPUScene glTF Viewer", 1280, 720, Examples_SDLWindowFlagsVulkan);
    RendererDesc rendererDesc{};
    auto [renderer0, app, device, swapchain] = Examples_InitVulkan(window, argc, argv, rendererDesc);
    UniquePtr<Renderer> renderer(renderer0, StlDeleter<Renderer>{GLOBAL_ALLOC});
    {
        GPUSceneDesc desc = scene.CalculateGPUSceneDesc(device->GetCapabilities());
        desc.lightBudget = std::max(desc.lightBudget, 1024u);
        GPUScene gpu(device.Get(), GLOBAL_ALLOC, desc);
        gpu.mLightSamplerType = GPUScene::LightSamplerType::Importance;

        FBlobDeserializer blobs = scene.GetBlobDeserializer();
        Vector<GeometryHandle> meshGeometry(scene.GetMeshes().size(), GeometryHandle{}, GLOBAL_ALLOC);
        for (size_t i = 0; i < scene.GetMeshes().size(); ++i)
        {
            GPUScene::Result r = gpu.Upload(&blobs, scene.GetMeshes()[i], meshGeometry[i]);
            CHECK_MSG(r == GPUScene::Result::InProgress, "Mesh {} upload rejected ({})", i, static_cast<int>(r));
        }

        Vector<GeometryHandle> curveGeometry(scene.GetCurves().size(), GeometryHandle{}, GLOBAL_ALLOC);
        for (size_t i = 0; i < scene.GetCurves().size(); ++i)
        {
            GPUScene::Result r = gpu.Upload(&blobs, scene.GetCurves()[i], curveGeometry[i]);
            CHECK_MSG(r == GPUScene::Result::InProgress, "Curve {} upload rejected ({})", i, static_cast<int>(r));
        }

        Vector<TextureHandle> textureIDMap(scene.GetTextures().size(), TextureHandle{}, GLOBAL_ALLOC);
        for (size_t i = 0; i < scene.GetTextures().size(); ++i)
        {
            FSerializedTexture const& texture = scene.GetTextures()[i];
            if (!texture.IsValid() || IsSceneEnvironmentTexture(scene, i))
                continue;
            GPUScene::Result r = gpu.Upload(&blobs, texture, textureIDMap[i]);
            CHECK_MSG(r == GPUScene::Result::InProgress, "Texture {} upload rejected ({})", i, static_cast<int>(r));
        }

        FLight const* environment = scene.GetEnvironmentLight();
        if (environment && environment->environmentMap && environment->environmentTexture != kInvalidTexture)
        {
            CHECK_MSG(environment->environmentTexture < scene.GetTextures().size(),
                      "Scene environment texture index out of range");
            FSerializedTexture const& environmentTextureDesc = scene.GetTextures()[environment->environmentTexture];
            ScopedArena environmentArena(GLOBAL_ALLOC, SceneTextureReadBudget(environmentTextureDesc));
            CHECK(environmentArena);
            AllocatorStack environmentAlloc(environmentArena);
            FTexture environmentTexture = ReadSceneTexture(scene, environmentTextureDesc, &environmentAlloc);
            CHECK_MSG(environmentTexture.GetFormat() == RHIResourceFormat::R32G32B32A32SignedFloat,
                      "Scene environment texture must be RGBA32F, got {}", environmentTexture.GetFormat());
            GPUScene::Result r = gpu.UploadEnvMap(environmentTexture);
            CHECK_MSG(r == GPUScene::Result::Ready, "Environment map upload rejected ({})", static_cast<int>(r));
        }

        gpu.Join();

        RendererUBO ubo{.adaptiveThreshold = 0.10f};
        ubo.ptSamplesPerPixel = 1;
        auto AuthorScene = [&]
        {
            scene.EnsureEnvironmentLight();
            auto instances = scene.GetInstances();
            auto materials = scene.GetMaterials();
            auto lights = scene.GetLights();
            auto tables = gpu.BeginScene(static_cast<uint32_t>(instances.size()),
                                         static_cast<uint32_t>(materials.size()),
                                         static_cast<uint32_t>(lights.size()));
            for (size_t i = 0; i < instances.size(); ++i)
            {
                FInstance const& src = instances[i];
                GeometryHandle geometry{};
                if (src.type == FInstanceType::Mesh)
                {
                    CHECK_MSG(src.resourceIndex < meshGeometry.size(), "Mesh instance references invalid mesh {}",
                              src.resourceIndex);
                    geometry = meshGeometry[src.resourceIndex];
                }
                else if (src.type == FInstanceType::Curve)
                {
                    CHECK_MSG(src.resourceIndex < curveGeometry.size(), "Curve instance references invalid curve {}",
                              src.resourceIndex);
                    geometry = curveGeometry[src.resourceIndex];
                }
                else
                    CHECK_MSG(false, "Unknown scene instance type {}", static_cast<uint32_t>(src.type));

                tables.instances[i] = InstanceDesc{
                    .geometry = geometry,
                    .transform = src.transform.transform,
                    .rotation = src.transform.rotation,
                    .scale = src.transform.scale,
                    .materialIndex = src.materialIndex};
            }
            for (size_t i = 0; i < materials.size(); ++i)
                FillGSMaterial(tables.materials[i], materials[i], textureIDMap, &gpu);
            for (size_t i = 0; i < lights.size(); ++i)
                FLightToGSLight(lights[i], tables.lights[i], &gpu, gpu.mLightSamplerType);
            gpu.EndScene(tables);
            gpu.BuildUBO(ubo);
        };
        AuthorScene();

        {
            ImmediateContext ctx(RHIDeviceQueueType::Compute, device.Get());
            auto* cmd = ctx.Get();
            cmd->Begin();
            auto tlasResult = gpu.BuildTLAS(cmd, /*update*/ false);
            cmd->End();
            if (tlasResult == GPUScene::TLASBuildResult::Built)
                ctx.Submit(), ctx.WaitIdle();
        }

        ExampleGPUSceneRenderState renderState{};
        ExampleInputState input{};
        CSDebugTextData hud[8]{};
        Examples_BeginControls(input);
        Examples_Text(input, hud[0], "GPUScene glTF viewer");
        Examples_Button(input, hud[1], "[Renderer]");
        Examples_SameLine(input);
        Examples_Button(input, hud[2], "[Pause]");
        Examples_Slider(input, Span<CSDebugTextData>(&hud[3], 3), "Resolution", renderState.renderScale, 0.10f, 1.0f, 0.05f);
        Examples_Text(input, hud[6], FExampleOrbitCamera::kControlsText);

        auto RecreateRenderer = [&]
        { renderer = ConstructUnique<Renderer>(GLOBAL_ALLOC, rendererDesc, device, swapchain, GLOBAL_ALLOC); };
        auto BuildGraph = [&](RHIExtent2D extent)
        { Examples_GPUSceneBuildRenderGraph(renderer.get(), &ubo, &gpu, renderState, hud, extent); };
        BuildGraph(renderer->GetSwapchainExtent());

        FExampleOrbitCamera camera{.center = {0.0f, 0.5f, 0.0f},
                                   .radius = 5.0f,
                                   .rot = normalize(angleAxis(radians(-18.0f), float3(1, 0, 0))),
                                   .zNear = 0.01f,
                                   .fovY = radians(50.0f)};
        ApplySceneCamera(scene, camera);

        ExampleFpsCounter fps;
        uint64_t lastTicks = SDL_GetTicksNS();
        bool paused = false;
        while (true)
        {
            Examples_BeginFrameInput(input);
            if (Examples_PollEvents(window, renderer.get(), swapchain, input))
                break;

            uint64_t now = SDL_GetTicksNS();
            float dt = static_cast<float>(now - lastTicks) / 1e9f;
            lastTicks = now;

            RHIExtent2D currentExtent = renderer->GetSwapchainExtent();
            if (currentExtent.x == 0u || currentExtent.y == 0u)
                continue;
            if (currentExtent.x != renderState.renderExtent.x || currentExtent.y != renderState.renderExtent.y)
            {
                device->WaitIdle();
                RecreateRenderer();
                BuildGraph(currentExtent);
            }

            Examples_BeginControls(input);
            Examples_Text(input, hud[0], "GPUScene glTF viewer");
            const bool toggleRenderer = Examples_Button(input, hud[1],
                fmt::format("[{}]", Examples_GPUSceneModeName(renderState.mode))) ||
                input.KeyPressed(SDLK_TAB);
            Examples_SameLine(input);
            const bool togglePause = Examples_Button(input, hud[2], paused ? "[Resume]" : "[Pause]") ||
                input.KeyPressed(SDLK_SPACE);
            const bool resolutionChanged = Examples_Slider(input, Span<CSDebugTextData>(&hud[3], 3),
                                                           "Resolution", renderState.renderScale, 0.10f, 1.0f, 0.05f);
            Examples_Text(input, hud[6], FExampleOrbitCamera::kControlsText);
            if (togglePause)
                paused = !paused;
            if (toggleRenderer || resolutionChanged)
            {
                if (toggleRenderer)
                    Examples_GPUSceneToggleMode(renderState);
                device->WaitIdle();
                RecreateRenderer();
                BuildGraph(renderState.renderExtent);
                ubo.ptAccumulatedFrames = 0u;
            }

            camera.aspect = static_cast<float>(renderState.renderExtent.x) / static_cast<float>(renderState.renderExtent.y);
            bool cameraMoved = !paused && camera.Update(input, dt);
            Examples_GPUSceneFillCameraUBO(ubo, renderer.get(), camera, renderState.config);
            Examples_Text(input, hud[7], fmt::format("{} meshes, {} instances, {} textures   {:.0f} FPS{}",
                                                     scene.GetMeshes().size(), scene.GetInstances().size(),
                                                     scene.GetTextures().size(), fps.Update(),
                                                     paused ? "   [PAUSED]" : ""));

            Examples_NewFrame(renderer.get());
            if (renderState.mode == ExampleGPUSceneRenderMode::PathTracer)
                ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;
            if (cameraMoved)
                ubo.ptAccumulatedFrames = 0u;
        }

        device->WaitIdle();
    }
    Examples_DestroyVulkan(window, renderer.release(), app, device, swapchain);
    return 0;
}
