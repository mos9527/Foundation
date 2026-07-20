// Demonstrates loading a glTF/GLB/FSCN into GPUScene and plays it!
#include <Editor/Runtime/Animation.hpp>
#include <Editor/Runtime/GPUScene.hpp>
#include <Renderer/Renderer.hpp>
#include <RenderCore/ImmediateContext.hpp> // For ImmediateReadback
#include <Core/Paths.hpp>
#include "Examples.hpp"
#include <argh.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

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

float ApertureRadiusFromFStop(float fStop, float sensorHeight, float fovY)
{
    if (fStop <= 0.0f || sensorHeight <= 0.0f)
        return 0.0f;

    float focalLength = (0.5f * sensorHeight) / std::tan(fovY * 0.5f);
    return focalLength / (2.0f * fStop);
}

void ApplySceneCamera(FImportedScene const& scene, RendererUBO& ubo, FExampleOrbitCamera& camera)
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
        ubo.aperture = src.lensEnabled
            ? ApertureRadiusFromFStop(src.fStop, src.sensorHeightMm * 1e-3f, src.fovY)
            : 0.0f;
        ubo.focalDistance = src.focusDistance;
        ubo.apertureBlades = src.apertureBlades;
        ubo.apertureRotation = src.apertureRotation;
        ubo.apertureRatio = src.apertureRatio;
        return;
    }
}
} // namespace

int main(int argc, char** argv)
{
    // --- Command line ---------------------------------------------------------------------
    // Positional: <scene path> (glTF/GLB/FSCN). Optional; defaults to Data/Assets/demo.glb.
    // Headless single-image render (no window, path traced):
    //   --headless              Render one image and exit (implied when -o/--output is given)
    //   -o, --output <path>     Output PNG path (default: render.png)
    //   -w, --width  <px>       Output width  (default: 1920)
    //       --height <px>       Output height (default: 1080)
    //   -s, --spp    <n>        Path-tracer samples to accumulate (default: 256)
    // (Shared -h/--help, -g/--gpu, -l/--list-gpus are handled by Examples_InitVulkan.)
    argh::parser cmdl;
    cmdl.add_params({"-o", "--output", "-w", "--width", "--height", "-s", "--spp", "--samples", "-g", "--gpu"});
    cmdl.parse(argc, argv);

    uint32_t renderWidth = 1920u;
    uint32_t renderHeight = 1080u;
    uint32_t sampleCount = 256u;
    cmdl({"-w", "--width"}, renderWidth) >> renderWidth;
    cmdl({"--height"}, renderHeight) >> renderHeight;
    cmdl({"-s", "--spp", "--samples"}, sampleCount) >> sampleCount;
    renderWidth = std::max(renderWidth, 1u);
    renderHeight = std::max(renderHeight, 1u);
    sampleCount = std::max(sampleCount, 1u);
    String outputPath = "render.png";
    if (auto out = cmdl({"-o", "--output"}))
        outputPath = out.str();
    const bool headless = cmdl[{"--headless"}] || static_cast<bool>(cmdl({"-o", "--output"}));

    String scenePathArg;
    if (auto positional = cmdl(1))
        scenePathArg = positional.str();

    SDL_Window* window = headless ? nullptr
                                  : SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("GPUScene glTF Viewer"), 1280, 720,
                                                     Examples_SDLWindowFlagsVulkan);
    RendererDesc rendererDesc{};
    if (headless)
        rendererDesc.threadCount = 0; // deterministic single-threaded submission for offscreen render
    auto [renderer0, app, device, surface, swapchain, presenter] = Examples_InitVulkan(window, argc, argv, rendererDesc);
    UniquePtr<Renderer> renderer(renderer0, StlDeleter<Renderer>{GLOBAL_ALLOC});

    // Examples_InitVulkan initializes PathsResolve's writable root and Android asset loader.
    // Resolve/import scene data after that so the temporary FSCN lands beside the pipeline cache.
    String scenePath = !scenePathArg.empty() ? scenePathArg : PathsResolve("Data/Assets/demo.glb");
    String scenePayloadPath = PrepareScenePayload(scenePath);
    MemoryMappedFile sceneFile(scenePayloadPath, MemoryMappedAccess::ReadOnly);
    FImportedScene scene(sceneFile, GLOBAL_ALLOC);
    LoadFSCN(scene);

    {
        GPUSceneDesc desc = CalculateSceneGPUDesc(scene, device->GetCapabilities());
        GPUScene gpu(device.Get(), GLOBAL_ALLOC, desc);

        FSceneGPUResources resources;
        UploadSceneResources(scene, gpu, resources);
        if (FLight const* environment = scene.GetEnvironmentLight(); environment && environment->HasEnvironmentTexture())
            UploadSceneEnvironment(scene, *environment, gpu);
        gpu.Join();

        RendererUBO ubo{.adaptiveThreshold = 0.10f};
        ubo.ptSamplesPerPixel = 1;
        CommitSceneToGPU(scene, gpu, resources, ubo, /*resetAccumulation*/ false);

        // Pose evaluation + CPU skinning/morphing run on a small pool, same as the editor.
        unsigned const hw = std::thread::hardware_concurrency();
        size_t const animWorkers = hw > 1u ? static_cast<size_t>(hw - 1u) : 1u;
        ThreadPool animJobs(animWorkers, ThreadPool::CalcTaskSize(std::max<size_t>(animWorkers * 4u, 64u)), GLOBAL_ALLOC,
                            "Anim");
        FAnimationRuntime anim;
        anim.Setup(scene, resources, animJobs.GetParallelForConcurrency());
        bool animPlaying = anim.playing;

        if (headless)
        {
            // --- Headless single-image render ------------------------------------------------
            // Build the path-tracer graph once at the requested resolution (no window/backbuffer,
            // so no debug-text/blit passes), accumulate `sampleCount` samples into the converged
            // image, read back the tonemapped RTV and write it to `outputPath`.
            bool renderPaused = false;
            RendererConfig cfg{};
            cfg.renderExtent = RHIExtent2D{renderWidth, renderHeight};
            cfg.ptRenderPaused = &renderPaused;
            RendererOutputs outputs{};
            renderer->BeginSetup();
            BuildPathTracerRenderGraph(renderer.get(), &ubo, &gpu, cfg, outputs);
            const ResourceHandle postprocess = Examples_InsertBasicTonemapPasses(renderer.get(), outputs, true);
            renderer->EndSetup();

            FExampleOrbitCamera camera{.center = {0.0f, 0.5f, 0.0f},
                                       .radius = 5.0f,
                                       .rot = normalize(angleAxis(radians(-18.0f), float3(1, 0, 0))),
                                       .zNear = 0.01f,
                                       .fovY = radians(50.0f)};
            ApplySceneCamera(scene, ubo, camera);
            camera.aspect = static_cast<float>(renderWidth) / static_cast<float>(renderHeight);
            camera.RefreshMatrices();

            // Scene + camera are static, so accumulation never resets: one converged sample/frame.
            ubo.ptSamplesPerPixel = 1;
            fmt::println("GPUSceneGLTF headless: rendering {}x{}, {} samples -> {}", renderWidth, renderHeight,
                         sampleCount, outputPath);
            for (uint32_t f = 0; f < sampleCount; ++f)
            {
                Examples_GPUSceneFillCameraUBO(ubo, renderer.get(), camera, cfg);
                Examples_NewFrame(renderer.get());
                ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;
                fmt::print("\r[PT] sample {}/{} ({:.0f}%)   ", f + 1, sampleCount,
                           100.0f * static_cast<float>(f + 1) / static_cast<float>(sampleCount));
                std::fflush(stdout);
            }
            fmt::println("");

            renderer->WaitForFrame();
            device->WaitIdle();

            // Read back the tonemapped RTV and write it out. Scoped so the readback releases its
            // device resources before the GPUScene/device teardown below.
            {
                auto* outputTex = renderer->DerefResource(postprocess).Get<RHITexture*>();
                const RHIExtent2D extent{renderWidth, renderHeight};
                const size_t dataSize = static_cast<size_t>(renderWidth) * renderHeight * 4;
                ImmediateReadback readback(device.Get(), dataSize + 16);
                readback.Begin();
                {
                    auto* cmd = readback.ctx.Get();
                    cmd->BeginTransition();
                    cmd->SetImageTransition(outputTex,
                        {.srcAccess = RHIResourceAccessBits::RenderTargetWrite,
                         .dstAccess = RHIResourceAccessBits::TransferRead,
                         .srcStage = RHIPipelineStageBits::RenderTargetOutput,
                         .dstStage = RHIPipelineStageBits::Transfer,
                         .srcImgLayout = RHITextureLayout::RenderTarget,
                         .dstImgLayout = RHITextureLayout::TransferSrc,
                         .srcImgRange = RHITextureSubresourceRange::Create()});
                    cmd->EndTransition();
                }
                char* pixels = readback.Readback(outputTex, dataSize,
                                                 RHITextureSubresourceLayer{.aspect = RHITextureAspectFlagBits::Color},
                                                 RHIOffset2D{}, extent);
                readback.End();
                readback.WaitIdle();
                Examples_DumpAndOpenImage(outputPath, extent, pixels);
            }

            device->WaitIdle();
            // Fall through to the shared teardown below so the GPUScene (this block's scope) is
            // destroyed before the device in Examples_DestroyVulkan.
        }
        else
        {
        ExampleGPUSceneRenderState renderState{};
        ExampleInputState input{};

        auto RecreateRenderer = [&]
        { renderer = ConstructUnique<Renderer>(GLOBAL_ALLOC, rendererDesc, device, swapchain, GLOBAL_ALLOC); };
        auto BuildGraph = [&](RHIExtent2D extent)
        { Examples_GPUSceneBuildRenderGraph(renderer.get(), &ubo, &gpu, renderState, input, extent); };
        BuildGraph(renderer->GetSwapchainExtent());

        FExampleOrbitCamera camera{.center = {0.0f, 0.5f, 0.0f},
                                   .radius = 5.0f,
                                   .rot = normalize(angleAxis(radians(-18.0f), float3(1, 0, 0))),
                                   .zNear = 0.01f,
                                   .fovY = radians(50.0f)};
        ApplySceneCamera(scene, ubo, camera);

        ExampleFpsCounter fps;
        uint64_t lastTicks = SDL_GetTicksNS();
        while (true)
        {
            Examples_BeginFrameInput(input);
            if (Examples_PollEvents(window, renderer.get(), surface, swapchain, input))
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
            Examples_Text(input, "GPUScene glTF viewer");
            const bool toggleRenderer = Examples_Button(input,
                fmt::format("[{}]", Examples_GPUSceneModeName(renderState.mode))) ||
                input.KeyPressed(SDLK_TAB);
            const bool resolutionChanged =
                Examples_Slider(input, "Resolution", renderState.renderScale, 0.10f, 1.0f, 0.05f, "x", false);
            Examples_Text(input, FExampleOrbitCamera::kControlsText);
            bool animScrubbed = false;
            if (anim.HasData())
            {
                animScrubbed = Examples_Slider(input, "Anim Time", anim.time, 0.0f, std::max(anim.duration, 0.01f),
                                               std::max(anim.duration / 100.0f, 0.01f), "s");
                Examples_SameLine(input);
                if (Examples_Button(input, animPlaying ? "[PAUSE]" : "[PLAY]"))
                    animPlaying = !animPlaying;
            }
            if (toggleRenderer || resolutionChanged)
            {
                if (toggleRenderer)
                    Examples_GPUSceneToggleMode(renderState);
                device->WaitIdle();
                RecreateRenderer();
                BuildGraph(renderState.renderExtent);
                ubo.ptAccumulatedFrames = 0u;
            }

            // A scrub applies its dropped time immediately (dirty), independent of playback.
            // Query CameraDrivesView before Begin(), which clears the scrub flag it reads. Kick
            // the per-skeleton pose evaluation now so it overlaps the camera work below.
            anim.playing = animPlaying;
            anim.dirty |= animScrubbed;
            bool const followAnim = anim.CameraDrivesView();
            anim.Begin(scene, &gpu, dt, animJobs, GLOBAL_ALLOC);

            camera.aspect = static_cast<float>(renderState.renderExtent.x) / static_cast<float>(renderState.renderExtent.y);
            bool cameraMoved = camera.Update(input, dt);
            // An animated scene camera overrides user navigation while it's playing/scrubbing.
            FTransform animCameraXf;
            if (followAnim && anim.GetCameraTransform(scene, animCameraXf))
            {
                float3 dir = animCameraXf.rotation * float3(0, 0, 1);
                camera.center = animCameraXf.transform - dir * camera.radius;
                camera.rot = animCameraXf.rotation;
                cameraMoved = true;
            }
            Examples_GPUSceneFillCameraUBO(ubo, renderer.get(), camera, renderState.config);
            Examples_Text(input, fmt::format("{} meshes, {} instances, {} textures   {:.0f} FPS",
                                             scene.GetMeshes().size(), scene.GetInstances().size(),
                                             scene.GetTextures().size(), fps.Update()));

            // Wait for the scheduled poses, apply rigid transforms + CPU-skin into the dynamic
            // ring, then re-author the scene so dynamic instances encode the current ring slot
            // (the graph's TLAS refit picks it up). Paused/held poses skip this.
            if (anim.End())
                CommitSceneToGPU(scene, gpu, resources, ubo, /*resetAccumulation*/ true);

            Examples_NewFrame(window, renderer.get(), presenter, surface, swapchain);
            if (renderState.mode == ExampleGPUSceneRenderMode::PathTracer)
                ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;
            if (cameraMoved)
                ubo.ptAccumulatedFrames = 0u;
        }
        } // else (interactive)

        device->WaitIdle();
    }
    Examples_DestroyVulkan(window, renderer.release(), app, device, surface, swapchain);
    return 0;
}
