// Demonstrates loading a glTF/GLB/FSCN into GPUScene with the Pathtracer integrator
#include <Core/Paths.hpp>
#include <Editor/Runtime/GPUScene.hpp>
#include <RenderCore/ImmediateContext.hpp> // For ImmediateReadback
#include <Renderer/Renderer.hpp>
#include <Renderer/Rasterizer.hpp>
#include <Renderer/Pathtracer.hpp>
#include <algorithm>
#include <argh.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>
#include "Examples.hpp"

using Foundation::Core::PathsResolve;
static constexpr const char* kTempScenePath = "Cache/GPUSceneGLTF.fscn";

String PrepareScenePayload(JobSystem* jobs, StringView path)
{
    String ext = std::filesystem::path(path.data()).extension().string();
    if (ext == ".fscn")
        return String(path);

    String scenePayloadPath = PathsResolve(kTempScenePath);
    std::filesystem::path scenePayloadDir = std::filesystem::path(scenePayloadPath).parent_path();
    if (!scenePayloadDir.empty())
        std::filesystem::create_directories(scenePayloadDir);

    {
        MemoryMappedFile sceneFile(scenePayloadPath, 256ull * 1024ull * 1024ull /* grows on demand */);
        FImportedScene writeScene(sceneFile, GLOBAL_ALLOC);
        LoadScene(jobs, path, writeScene, GLOBAL_ALLOC);
    }
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
        ubo.aperture =
            src.lensEnabled ? ApertureRadiusFromFStop(src.fStop, src.sensorHeightMm * 1e-3f, src.fovY) : 0.0f;
        ubo.focalDistance = src.focusDistance;
        ubo.apertureBlades = src.apertureBlades;
        ubo.apertureRotation = src.apertureRotation;
        ubo.apertureRatio = src.apertureRatio;
        return;
    }
}

int main(int argc, char** argv)
{
    // --- Command line ---------------------------------------------------------------------
    // Positional: <scene path> (glTF/GLB/FSCN). Optional; defaults to Data/Assets/demo.glb.
    // Headless single-image render (no window, path traced):
    //   --headless               Render one image and exit (implied when -o/--output is given)
    //   -o, --output  <path>     Output PNG path (default: render.png)
    //   -w, --width   <px>       Output width  (default: 1920)
    //       --height  <px>       Output height (default: 1080)
    //   -s, --samples <n>        Path-tracer samples to accumulate (default: 256)
    // (Shared -h/--help, -g/--gpu, -l/--list-gpus are handled by Examples_InitVulkan.)
    argh::parser cmdl;
    cmdl.add_params({"-o", "--output", "-w", "--width", "--height", "-s", "--samples", "-g", "--gpu"});
    cmdl.parse(argc, argv);

    uint32_t renderWidth = 1920u;
    uint32_t renderHeight = 1080u;
    uint32_t sampleCount = 256u;
    cmdl({"-w", "--width"}, renderWidth) >> renderWidth;
    cmdl({"--height"}, renderHeight) >> renderHeight;
    cmdl({"-s", "--samples"}, sampleCount) >> sampleCount;
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

    auto ctx = Examples_InitVulkan(window, argc, argv, RendererDesc{});
    // Prepare scene file
    String path =
        PrepareScenePayload(ctx.jobs.get(), !scenePathArg.empty() ? scenePathArg : PathsResolve("Data/Assets/demo.glb"));
    MemoryMappedFile sceneFile(path, MemoryMappedAccess::ReadOnly);
    FImportedScene scene(sceneFile, GLOBAL_ALLOC);
    LoadFSCN(scene);
    {
        GPUSceneDesc desc = CalculateSceneGPUDesc(scene, ctx.device->GetCapabilities());
        GPUScene gpu(ctx.device.Get(), ctx.jobs.get(), GLOBAL_ALLOC, desc);

        FSceneGPUResources resources;
        UploadSceneResources(scene, gpu, resources);
        if (FLight const* environment = scene.GetEnvironmentLight();
            environment && environment->HasEnvironmentTexture())
            UploadSceneEnvironment(scene, *environment, gpu);

        RendererUBO ubo{.adaptiveThreshold = 0.10f};
        ubo.ptSamplesPerPixel = 1;

        RendererConfig cfg{};        
        FExampleOrbitCamera camera{.center = {0.0f, 0.5f, 0.0f},
                                   .radius = 5.0f,
                                   .rot = normalize(angleAxis(radians(-18.0f), float3(1, 0, 0))),
                                   .zNear = 0.01f,
                                   .fovY = radians(50.0f)};
        RendererOutputs outputs{};
        ApplySceneCamera(scene, ubo, camera);
        CommitSceneToGPU(scene, gpu, resources, ubo);
        if (headless)
        {
            ctx.renderer->BeginSetup();
            cfg.renderExtent = RHIExtent2D{renderWidth, renderHeight};
            auto gpuResources = CreateGPUSceneRendererResources(ctx.renderer.get(), &gpu);
            Example_BuildExamplePathTracerRenderGraph(ctx.renderer.get(), &ubo, gpuResources, cfg, outputs);
            const ResourceHandle output = Examples_BuildTonemappingPass(ctx.renderer.get(), outputs, false);
            ctx.renderer->EndSetup();
            // Accumalate
            gpu.Join();
            for (uint32_t f = 0; f < sampleCount; ++f)
            {
                Examples_UpdateCameraUBO(ubo, ctx.renderer.get(), camera, cfg);
                Examples_NewFrame(ctx.renderer.get());
                ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;
                fmt::print("\rSample {}/{} ({:.0f}%)   ", f + 1, sampleCount,
                           100.0f * static_cast<float>(f + 1) / static_cast<float>(sampleCount));
                std::fflush(stdout);
            }
            fmt::println("");
            ctx.renderer->WaitForFrame();
            // Readback and save the image
            {
                auto* outputTex = ctx.renderer->DerefResource(output).Get<RHITexture*>();
                const size_t dataSize = static_cast<size_t>(renderWidth) * renderHeight * 4;
                ImmediateReadback readback(ctx.device.Get(), dataSize + 16);
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
                                                 RHIOffset2D{}, cfg.renderExtent);
                readback.End();
                readback.WaitIdle();
                Examples_DumpAndOpenImage(outputPath, cfg.renderExtent, pixels);
            }
        }
        else
        {
            ExampleInputState input{};
            ExampleFpsCounter fps;
            uint64_t t0 = SDL_GetTicksNS();            
#if defined(__ANDROID__)
            // TDRs are...expected otherwise
            float scaling = 0.10f;
#else
            float scaling = 1.00f;
#endif
            float maxSamples = 0u;
            bool paused = false;
            ExampleRenderer renderer = ExampleRenderer::PathTracer;
            cfg.ptRenderPaused = &paused;
            while (true)
            {
                uint64_t t1 = SDL_GetTicksNS();
                float dt = static_cast<float>(t1 - t0) / 1e9f;
                t0 = t1;
                Examples_BeginFrameInput(input);
                if (Examples_PollEvents(window, ctx, input))
                    break;
                if (camera.Update(input, dt) /* moved */)
                    ubo.ptAccumulatedFrames = 0u, paused = false;
                else
                    if (!paused) 
                        ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;
                if (input.wantResizeOrRebuild)
                {
                    input.wantResizeOrRebuild = paused = false;
                    ubo.ptAccumulatedFrames = 0u;
                    Examples_ResetRenderer(ctx, RendererDesc{});
                    ctx.renderer->BeginSetup();      
                    renderWidth = ctx.renderer->GetSwapchainExtent().x;
                    renderHeight = ctx.renderer->GetSwapchainExtent().y;
                    cfg.renderExtent = RHIExtent2D{float2(renderWidth, renderHeight) * scaling};
                    auto gpuResources = CreateGPUSceneRendererResources(ctx.renderer.get(), &gpu);
                    if (renderer == ExampleRenderer::PathTracer)
                        Example_BuildExamplePathTracerRenderGraph(ctx.renderer.get(), &ubo, gpuResources, cfg, outputs);
                    else
                        Example_BuildExampleRasterRenderGraph(ctx.renderer.get(), &ubo, gpuResources, cfg, outputs);
                    Examples_BuildTonemappingPass(ctx.renderer.get(), outputs, true);
                    RenderUtils::createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", Examples_HudLines(input));
                    ctx.renderer->EndSetup();
                }                
                Examples_UpdateCameraUBO(ubo, ctx.renderer.get(), camera, cfg);
                CommitSceneToGPU(scene, gpu, resources, ubo);
                // Debug Text
                Examples_Text(input,
                              fmt::format("{} meshes, {} instances, {} textures   {:.0f} FPS {} Samples", scene.GetMeshes().size(),
                                          scene.GetInstances().size(), scene.GetTextures().size(), fps.Update(), ubo.ptAccumulatedFrames));
                if (Examples_RendererSwitchButton(input, renderer))
                    input.wantResizeOrRebuild = true;
                input.wantResizeOrRebuild |= Examples_Slider(input, "Resolution", scaling, 0.10f, 1.0f, 0.05f, "x", false);
                if (Examples_Slider(input, "Samples", maxSamples, 0.0f, 128.0f, 1.0f))
                {
                    ubo.ptAccumulatedFrames = 0u;
                    paused = false;
                }
                // Auto pause
                if (maxSamples > 0.0f && ubo.ptAccumulatedFrames >= maxSamples)
                    paused = true;
                Examples_NewFrame(window, ctx);
            }
        }
    }
    Examples_DestroyVulkan(window, ctx);
    return 0;
}
