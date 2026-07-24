// Demonstrates GPU-generated dynamic geometry and CPU-generated static geo usage.    
#include <Renderer/Mesh.hpp>
#include <algorithm>
#include "Examples.hpp"
#include <Renderer/Rasterizer.hpp>
#include <Renderer/ProgressivePathtracer.hpp>
namespace
{
    constexpr uint32_t kWaterQuads = 64u;
    constexpr uint32_t kWaterVerts = (kWaterQuads + 1u) * (kWaterQuads + 1u);
    constexpr uint32_t kWaterIndices = kWaterQuads * kWaterQuads * 6u;
    constexpr float kWaterExtent = 8.0f;

    constexpr uint32_t kGroundVerts = 4u;
    constexpr uint32_t kGroundIndices = 6u;
    constexpr float kGroundExtent = 16.0f;
    constexpr float kGroundY = -0.5f;

    struct GerstnerState
    {
        GeometryHandle water;
        float time{0.0f};
        float amplitude{1.0f};
    };

    struct GerstnerPush
    {
        uint32_t vertexOffset;
        uint32_t indexOffset;
        uint32_t gridQuads;
        float extent;
        float time;
        float amplitudeScale;
    };

    // GPU Side geometry update
    // Demonstrates per-frame invocation through a CS pass
    void BuildGerstnerPass(Renderer* renderer, RendererResources const& resources, GerstnerState const* state)
    {
        renderer->CreatePass(
            "GPU Gerstner Water", RHIDeviceQueueType::Compute, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "main",
                              PathsResolve("Data/Shaders/DynamicGerstner.spv"));
                r->BindBufferUnordered(self, resources.dynamicPrimitiveBuffer, RHIPipelineStageBits::ComputeShader,
                                       "dynamicPrimitives");
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(GerstnerPush));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                uint32_t meshOffset = 0;
                uint32_t meshType = 0;
                GSInstanceFlags meshFlags;
                resources.scene->ResolveGeometry(state->water, meshOffset, meshType, meshFlags);
                CHECK(meshFlags & GSInstanceFlagsBits::Dynamic);
                GerstnerPush push{.vertexOffset = static_cast<uint32_t>(meshOffset + sizeof(GSMesh)),
                                  .indexOffset = static_cast<uint32_t>(meshOffset + sizeof(GSMesh) + kWaterVerts * sizeof(FQVertex)),
                                  .gridQuads = kWaterQuads,
                                  .extent = kWaterExtent,
                                  .time = state->time,
                                  .amplitudeScale = state->amplitude};
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, push);
                r->CmdDispatch(self, cmd, {std::max(kWaterVerts, kWaterQuads * kWaterQuads), 1, 1});
            });
    }
    void CommitDemoScene(GPUScene& gpu, GeometryHandle water, GeometryHandle ground, RendererUBO& ubo)
    {
        auto tables = gpu.BeginScene(2, 2, 2);
        tables.instances[0] = GSInstance{
            .transform = float3(0, 0, 0),
            .rotation = quat(0, 0, 0, 1),
            .scale = float3(1, 1, 1),
            .materialIndex = 0,
            .resourceIndex = water.index,
            .type = kGSInstanceTypeMesh,
        };
        tables.instances[1] = GSInstance{
            .transform = float3(0, 0, 0),
            .rotation = quat(0, 0, 0, 1),
            .scale = float3(1, 1, 1),
            .materialIndex = 1,
            .resourceIndex = ground.index,
            .type = kGSInstanceTypeMesh,
        };

        tables.materials[0] = GSMaterial{};
        tables.materials[0].baseColorFactor = float4(0.05f, 0.25f, 0.35f, 1.0f);
        tables.materials[0].metallicFactor = 0.0f;
        tables.materials[0].roughnessFactor = 1.0f;
        tables.materials[0].transmissionFactor = 1.0f;
        tables.materials[0].ior = 1.5f;

        tables.materials[1] = GSMaterial{};
        tables.materials[1].baseColorFactor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        tables.materials[1].metallicFactor = 0.0f;
        tables.materials[1].roughnessFactor = 0.9f;
        tables.materials[1].ior = 1.5f;

        tables.lights[0] = GSLight{
            .flags = kGSLightTypeEnvironment,
            .color = float3(0.45f, 0.55f, 0.7f),
            .power = 1.0f,
        };
        tables.lights[1] = GSLight{.flags = kGSLightTypePoint,
                                   .color = float3(1.0f, 0.96f, 0.9f),
                                   .power = 33.0f,
                                   .position = float3(0, 1, 0),
                                   .params = float4(.05f)};
        gpu.EndScene(tables);
        gpu.UpdateUBO(ubo);
    }

    void RebuildGraph(ExampleVulkanContext& ctx, RendererUBO& ubo, GPUScene& gpu, RendererConfig& cfg,
                      RendererOutputs& outputs, ExampleInputState& input, GerstnerState const& gerstner, ExampleRenderer renderer)
    {
        Examples_ResetRenderer(ctx, RendererDesc{});
        ctx.renderer->BeginSetup();
        cfg.renderExtent = ctx.renderer->GetSwapchainExtent();
        ubo.ptMaxBounces = 2u;
        auto resources = CreateGPUSceneRendererResources(ctx.renderer.get(), &gpu);
        BuildGPUSceneHostUpdatePass(ctx.renderer.get(), resources);
        BuildGerstnerPass(ctx.renderer.get(), resources, &gerstner);
        if (renderer == ExampleRenderer::PathTracer)
            BuildProgressivePathTracerRenderGraph(ctx.renderer.get(), &ubo, resources, cfg, outputs);
        else
            Example_BuildExampleRasterRenderGraph(ctx.renderer.get(), &ubo, resources, cfg, outputs);
        Examples_BuildTonemappingPass(ctx.renderer.get(), outputs, true);
        RenderUtils::createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", Examples_HudLines(input));
        ctx.renderer->EndSetup();
    }
} // namespace

int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("GPUScene Dynamic Geometry"), 1280, 720,
                                          Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(window, argc, argv, RendererDesc{});   

    GPUSceneDesc desc{};
    desc.primitiveBudget = 64u * 1024u;
    desc.dynamicGeometryBudget = 256u * 1024u;
    desc.dynamicStagingBudget = 256u * 1024u;
    desc.instanceBudget = 8;
    desc.materialBudget = 8;
    desc.lightBudget = 8;
    desc.geometryBudget = 8;
    desc.tlasInstanceBudget = 8;
    GPUScene gpu(ctx.device.Get(), ctx.jobs.get(), GLOBAL_ALLOC, desc);
    GeometryHandle water{};
    GeometryHandle ground{};
    {
        FImportedMesh groundMesh = Examples_MakePlaneMesh(kGroundExtent, kGroundY, GLOBAL_ALLOC);
        groundMesh.Optimize();
        groundMesh.ClusterizeDAG();

        CHECK(gpu.Upload(groundMesh, ground) == GPUScene::Result::Ready);
        CHECK(gpu.Allocate(kWaterVerts, kWaterIndices, water, true) == GPUScene::Result::Ready);
    }

    RendererUBO ubo{};
    RendererConfig cfg{.cullFlags{CullFlagsBits::Frustum | CullFlagsBits::Backface}};
    GerstnerState gerstner{.water = water};
    if (!ctx.device->GetCapabilities().raytracingInline)
        cfg.viewFlags &= ~ViewFlagsBits::EnableRasterRTShadows;

    FExampleOrbitCamera camera{.center = {0.0f, 0.0f, 0.0f},
                               .radius = 2.0f,
                               .rot = normalize(angleAxis(radians(-28.0f), float3(1, 0, 0))),
                               .zNear = 0.01f,
                               .fovY = radians(45.0f)};
    RendererOutputs outputs{};
    ExampleInputState input{};
    ExampleFpsCounter fps;
    float amplitude = 1.0f;
    float paused = 0.0f;
    float time = 0.0f;
    ExampleRenderer renderer = ExampleRenderer::PathTracer;
    uint64_t t0 = SDL_GetTicksNS();

    // CPU side data
    while (true)
    {
        uint64_t t1 = SDL_GetTicksNS();
        float dt = static_cast<float>(t1 - t0) / 1e9f;
        t0 = t1;
        Examples_BeginFrameInput(input);
        if (Examples_PollEvents(window, ctx, input))
            break;
        if (paused < 0.5f)
            time += dt;
        gerstner.time = time;
        gerstner.amplitude = amplitude;

        if (input.wantResizeOrRebuild)
        {
            input.wantResizeOrRebuild = false;
            RebuildGraph(ctx, ubo, gpu, cfg, outputs, input, gerstner, renderer);
        }

        gpu.BeginDynamicGeometryUpdate();
        // NOTE: You don't need to do this every frame. This is to demonstrate dynamic geometry updates from host.
        //       Note that providing indices to be updated/flagging it as true triggers rebuilds. Leaving them empty/false
        //       singlals that refit can be used.
        gpu.UpdateDynamicGeometryGPU(water, true, true);
        gpu.EndDynamicGeometryUpdate();

        if (paused < 0.5f)
            ubo.ptAccumulatedFrames = 0u;
        else
            ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;

        if (camera.Update(input, dt))
            ubo.ptAccumulatedFrames = 0u;

        Examples_UpdateCameraUBO(ubo, ctx.renderer.get(), camera, cfg);
        CommitDemoScene(gpu, water, ground, ubo);

        Examples_Text(input,
                      fmt::format("Gerstner Water | {:.0f} FPS | refit {} rebuild {}", fps.Update(),
                                  gpu.GetDynamicRefitCount(), gpu.GetDynamicRebuildCount()));
        Examples_Text(input, FExampleOrbitCamera::kControlsText);
        if (Examples_RendererSwitchButton(input, renderer))
            input.wantResizeOrRebuild = true;
        Examples_Slider(input, "Amplitude", amplitude, 0.0f, 2.0f, 0.05f, "x");
        Examples_Slider(input, "Paused", paused, 0.0f, 1.0f, 1.0f, "");
        Examples_NewFrame(window, ctx);
    }

    Examples_DestroyVulkan(window, ctx);
    return 0;
}
