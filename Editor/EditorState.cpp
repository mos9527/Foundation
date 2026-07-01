#include <Core/Paths.hpp>
#include <cmath>
#include <RenderUtils/PSFullscreen.hpp>
#include <Renderer/Postprocess.hpp>
#include "EditorGizmos.hpp"
#include "EditorState.hpp"
#include <imgui.h>
#include <ImGuizmo.h>
using namespace RenderUtils;
using Foundation::Core::PathsResolve;

EditorState GEditor;
static ResourceHandle sPickResultBuffer;
static RendererOutputs sRenderOutputs;
static int2 sPickingPixel;
static bool sPickingDoubleClick = false;

static RHIExtent2D ClampViewportExtent(RHIExtent2D extent)
{
    return {std::max(extent.x, 16u), std::max(extent.y, 16u)};
}

static RHIExtent2D ViewportExtentFromImGui()
{
    ImGuiIO& io = ImGui::GetIO();
    uint32_t w = static_cast<uint32_t>(std::round(std::max(io.DisplaySize.x * io.DisplayFramebufferScale.x, 16.0f)));
    uint32_t h = static_cast<uint32_t>(std::round(std::max(io.DisplaySize.y * io.DisplayFramebufferScale.y, 16.0f)));
    return {w, h};
}

static void UpdateBackbufferViewport()
{
    ImGuiIO& io = ImGui::GetIO();
    GEditor.viewport.contentMin = ImVec2(0.0f, 0.0f);
    GEditor.viewport.contentMax = io.DisplaySize;
    GEditor.viewport.renderExtent = ViewportExtentFromImGui();
    GEditor.viewport.visible = io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f;
}

void DestroyEditorRenderer(FContext* context)
{
    context = context ? context : GContext;
    if (!context || !context->renderer)
        return;

    EditorGizmos::Shutdown();
    Destruct(context->allocator, context->renderer);
    context->renderer = nullptr;
}

static Renderer* BeginEditorRendererSetup(FContext* context, uint32_t threadCount)
{
    DestroyEditorRenderer(context);

    RendererDesc desc{};
    desc.asyncCompute = true;
    desc.threadCount = threadCount;
    desc.pipelineCache = context->psoCache.Get();
    auto* renderer = context->renderer = Construct<Renderer>(context->allocator, desc, context->device,
                                                             context->swapchain, context->allocator);
    renderer->BeginSetup();
    return renderer;
}

static void RefreshPostprocessState(RHIExtent2D renderExtent)
{
    GEditor.postprocessGlobals.camEV = GEditor.shaderGlobals.camEV;
    GEditor.postprocessGlobals.dbgShowOutline = GEditor.showImGui ? 1u : 0u;
    GEditor.postprocessGlobals.outlineInstanceId = GEditor.selectedInstance;
    GEditor.postprocessGlobals.ptAccumulatedFrames = GEditor.shaderGlobals.ptAccumulatedFrames;
    // fbWidth/fbHeight = display (output) extent for the blit pass UV mapping
    RHIExtent2D displayExtent = ClampViewportExtent(GEditor.viewport.renderExtent);
    GEditor.postprocessGlobals.fbWidth = static_cast<float>(displayExtent.x);
    GEditor.postprocessGlobals.fbHeight = static_cast<float>(displayExtent.y);
    // renderWidth/renderHeight = internal (scaled) render resolution
    GEditor.postprocessGlobals.renderWidth = static_cast<float>(renderExtent.x);
    GEditor.postprocessGlobals.renderHeight = static_cast<float>(renderExtent.y);
    GEditor.postprocessGlobals.viewLutIndex =
        Postprocess::ResolvePostprocessViewLutIndex(GEditor.viewLUTSdrHandle, GEditor.viewLUTHdrHandle, GContext->enableHDR);
    GEditor.postprocessGlobals.dbgViewFlags = GEditor.rendererConfig.viewFlags;
}

static void InsertEditorPostprocessPasses(FContext* context, Renderer* renderer, RendererOutputs& outputs, bool isRendering)
{
    CHECK(context);
    CHECK(renderer);
    CHECK(context->gpuScene);    
    CHECK_MSG(outputs.instanceID != kInvalidHandle, "Renderer outputs missing instance ID texture");
    if (outputs.extent.x == 0u || outputs.extent.y == 0u)
        outputs.extent = ClampViewportExtent(renderer->GetSwapchainExtent());
    uint32_t w = outputs.extent.x;
    uint32_t h = outputs.extent.y;
    const RHIResourceFormat postprocessFormat = Postprocess::GetPostprocessOutputFormat(context->enableHDR);
    RefreshPostprocessState(outputs.extent);

    auto PostprocessGlobals = renderer->CreateResource(
        "Postprocess UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(PostprocessUBO)});
    renderer->CreatePass(
        "Postprocess UBO Update", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r) { r->BindBufferCopyDst(self, PostprocessGlobals); },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* ubo = r->DerefResource(PostprocessGlobals).Get<RHIBuffer*>();
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(GEditor.postprocessGlobals)));
        });

    auto PostprocessBuffer = renderer->CreateResource(
        "Postprocess",
        RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                  RHITextureUsageBits::SampledImage |
                                  RHITextureUsageBits::TransferSource,
                       .extent = {w, h, 1},
                       .format = postprocessFormat});
    sPickResultBuffer = renderer->CreateResource("Pick Result Buffer",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Readback,
                                   .hostAccess = RHIResourceHostAccess::ReadWrite,
                                   .coherent = true},
                      .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = sizeof(uint32_t)});
    renderer->CreatePass(
        "Pick Result Init", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r) { r->BindBufferCopyDst(self, sPickResultBuffer); },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* pickResult = r->DerefResource(sPickResultBuffer).Get<RHIBuffer*>();
            cmd->FillBuffer(pickResult, ~0u);
        });
    GEditor.postprocessOutput = PostprocessBuffer;
    auto LUTSampler = renderer->CreateSampler({.addressMode = {
                                                   .u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                                   .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                                   .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                               }});

    if (GEditor.rendererMode == ERendererMode::PathTracer)
    {
        createPSFullscreenPassRTV(
            renderer, "Editor Postprocess PT", PostprocessBuffer,
            RHITextureViewDesc{.format = postprocessFormat,
                               .range = RHITextureSubresourceRange::Create()},
            {w, h},
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                              PathsResolve("Data/Shaders/EPSPostprocessPT.spv"));
                r->BindTextureSRV(self, outputs.diffuse, "diffuseTex", RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create()});
                ResourceHandle specular = outputs.specular != kInvalidHandle ? outputs.specular : outputs.diffuse;
                r->BindTextureSRV(self, specular, "specularTex", RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create()});
                r->BindBufferUniform(self, PostprocessGlobals, RHIPipelineStageBits::FragmentShader, "globalParams");
                r->BindDescriptorSet(self, "textures3D", context->gpuScene->GetTexture3DPool()->GetDescriptorSetLayout());
                r->BindTextureSampler(self, LUTSampler, "lutSampler");
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdBindDescriptorSet(self, cmd, "textures3D", context->gpuScene->GetTexture3DPool()->GetDescriptorSet());
            });
    }
    else
    {
        auto CopySampler = renderer->CreateSampler({});
        if (outputs.debugOutput != kInvalidHandle)
        {
            createPSFullscreenPassRTV(
                renderer, "Editor Debug Blit Raster", PostprocessBuffer,
                RHITextureViewDesc{.format = postprocessFormat,
                                   .range = RHITextureSubresourceRange::Create()},
                [=](PassHandle self, Renderer* r)
                {
                    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", PathsResolve("Data/Shaders/PSCopy.spv"));
                    r->BindTextureSampler(self, CopySampler, "sampler");
                    r->BindTextureSRV(self, outputs.debugOutput, "srcTexture", RHIPipelineStageBits::FragmentShader,
                                      RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                                                         .range = RHITextureSubresourceRange::Create()});
                });
        }
        else
        {
        createPSFullscreenPassRTV(
            renderer, "Editor Postprocess Raster", PostprocessBuffer,
            RHITextureViewDesc{.format = postprocessFormat,
                               .range = RHITextureSubresourceRange::Create()},
            {w, h},
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                              PathsResolve("Data/Shaders/EPSPostprocess.spv"));
                CHECK_MSG(outputs.diffuse != kInvalidHandle, "Raster postprocess missing diffuse output");
                ResourceHandle specular = outputs.specular != kInvalidHandle ? outputs.specular : outputs.diffuse;
                r->BindTextureSRV(self, outputs.diffuse, "diffuseTex", RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create(
                                                         RHITextureAspectFlagBits::Color, 0, 1)});
                r->BindTextureSRV(self, specular, "specularTex", RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create(
                                                         RHITextureAspectFlagBits::Color, 0, 1)});
                r->BindBufferUniform(self, PostprocessGlobals, RHIPipelineStageBits::FragmentShader, "globalParams");
                r->BindDescriptorSet(self, "textures3D", context->gpuScene->GetTexture3DPool()->GetDescriptorSetLayout());
                r->BindTextureSampler(self, LUTSampler, "lutSampler");
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdBindDescriptorSet(self, cmd, "textures3D", context->gpuScene->GetTexture3DPool()->GetDescriptorSet());
            });
        }
    }

    auto BlitSampler = renderer->CreateSampler({
        .addressMode = {
            .u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
            .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
            .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
        },
        .filter = {RHIDeviceSampler::SamplerDesc::Filter::Linear, RHIDeviceSampler::SamplerDesc::Filter::Linear},
    });
    createPSFullscreenPass(
        renderer, "Editor Blit",
        [=](PassHandle self, Renderer* r)
        {
            const int kViewInteractive = 1 << 0; 
            uint flags{};
            if (!isRendering)
                flags |= kViewInteractive;
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", PathsResolve("Data/Shaders/EPSBlit.spv"), AsBytes(AsSpan(flags)));
            r->BindTextureSRV(self, PostprocessBuffer, "displayImage", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = postprocessFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSampler(self, BlitSampler, "displaySampler");
            r->BindTextureSRV(self, outputs.instanceID, "instanceIDBuffer", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferUnordered(self, sPickResultBuffer, RHIPipelineStageBits::FragmentShader, "pickResult");
            r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(int2));
            r->BindBufferUniform(self, PostprocessGlobals, RHIPipelineStageBits::FragmentShader, "globalParams");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, sPickingPixel);
        });
    auto [screenWidth, screenHeight] = renderer->GetSwapchainExtent();
    if (!isRendering)
        EditorGizmos::InsertPass(renderer, outputs.depth, {screenWidth, screenHeight});
}

static void EndEditorRendererSetup(Renderer* renderer)
{
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", false, FSetupDefault{});
    renderer->EndSetup();
}

static void SetupIdleRenderer(FContext* context)
{
    auto* renderer = BeginEditorRendererSetup(context, 0u);
    EndEditorRendererSetup(renderer);
}

static void SetupSceneRenderer(FContext* context, RendererOutputs& outOutputs)
{
    auto* renderer = BeginEditorRendererSetup(context, 4u);
    RHIExtent2D renderExtent = ClampViewportExtent(renderer->GetSwapchainExtent());
    GEditor.viewport.renderExtent = renderExtent; // display extent (for mouse mapping)
    // Apply render resolution scale for internal render targets
    RHIExtent2D scaledExtent = {
        std::max(16u, static_cast<uint32_t>(renderExtent.x * GEditor.renderResolutionScale)),
        std::max(16u, static_cast<uint32_t>(renderExtent.y * GEditor.renderResolutionScale))
    };
    GEditor.rendererConfig.renderExtent = scaledExtent;
    GEditor.rendererConfig.ptRenderPaused = &GEditor.renderTask.renderPaused;
    if (GEditor.rendererMode == ERendererMode::PathTracer)
        BuildPathTracerRenderGraph(renderer, &GEditor.shaderGlobals, context->gpuScene, GEditor.rendererConfig, outOutputs);
    if (GEditor.rendererMode == ERendererMode::Raster)
        BuildRasterRenderGraph(renderer, &GEditor.shaderGlobals, context->gpuScene, GEditor.rendererConfig, outOutputs);
    InsertEditorPostprocessPasses(context, renderer, outOutputs, GEditor.rendererConfig.isRendering);
    EndEditorRendererSetup(renderer);
}

static void FInitEnter()
{
    // Loading a scene installs it immediately and switches to FERunningEnter (it renders
    // while it streams in). If no file installs a scene, fall back to the no-scene branch.
    for (size_t i = 0; i < GContext->files.size(); i++)
        HandleFile(GContext->files[i]);
    // Startup file opens are not inside a renderer execute frame, so drain the queue now.
    PumpDeferredSceneLoad();
    // Handle Renderer Settings passed from context (cmd lines)
    GEditor.shaderGlobals.ptFireflyClamp = GContext->rendererSettings.energyClampOverride;
    GEditor.rendererMode = static_cast<ERendererMode>(GContext->rendererSettings.defaultRenderer);
    GEditor.renderResolutionScale = GContext->rendererSettings.renderScale;
    if (GEditor.state == FEInitEnter)
    {
        LOG(Editor, LogInfo, "No scene path provided, starting with empty editor");
        SetupIdleRenderer(GContext);
        GEditor.state = FENoScene;
    }
}

// No scene installed yet: render the ImGui shell only. A drag-and-drop / file open installs
// a scene and transitions to FERunningEnter.
static void FNoScene()
{
    auto* renderer = GContext->renderer;
    if (!renderer)
    {
        SetupIdleRenderer(GContext);
        renderer = GContext->renderer;
    }
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    {
        EditorDockSpaceAndMenuBar();
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs;

        ImGui::Begin("##NoSceneOverlay", nullptr, flags);
        {
            const char* labels[] = {"  NO SCENE  ", "", " DRAG & DROP TO LOAD ", ""};
            int blink = (SDL_GetTicks() / 1000) & 3;
            ImFillText(labels[blink], ImGui::GetColorU32(ImGuiCol_TextDisabled));
        }
        ImGui::End();
    }
    float dt = ImGui::GetIO().DeltaTime;
    GEditor.camera.aspect = GContext->swapchain->GetAspectRatio();
    GEditor.camera.UpdateMovement(dt);
    GEditor.camera.Update({});
    renderer->ExecuteFrame();
    renderer->EndExecute();
}

static void FRunningEnter()
{
    sPickResultBuffer = kInvalidHandle;
    sPickingPixel = {-1, -1};
    sPickingDoubleClick = false;
    // The old renderer owns views into the current swapchain; release them before recreating the swapchain.
    DestroyEditorRenderer(GContext);
    UpdateSwapchain(GContext);
    // Invalidate stale readback handles before rebuilding the renderer
    sRenderOutputs = {};
    SetupSceneRenderer(GContext, sRenderOutputs);
    GEditor.state = FERunning;
}

static void FRunning()
{
    auto* renderer = GContext->renderer;
    // New frame
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    UpdateBackbufferViewport();
    if (GEditor.showImGui)
    {
        EditorDockSpaceAndMenuBar();
        FHierarchyPanel();
        FRunningImGui();
    }
    // Global param update
    float dt = ImGui::GetIO().DeltaTime;
    // Decide before BeginAnimationUpdate, which clears the scrub flag this query reads.
    bool const followAnimatedCamera = AnimatedCameraDrivesView();
    // Kick the per-skeleton pose evaluation now so it overlaps the camera/UBO globals update below.
    // EndAnimationUpdate (further down) waits for it before skinning + committing the scene.
    BeginAnimationUpdate(dt);
    RHIExtent2D displayExtent = ClampViewportExtent(GEditor.viewport.renderExtent);
    // Apply render resolution scale for internal render targets
    RHIExtent2D renderExtent = {
        std::max(16u, static_cast<uint32_t>(displayExtent.x * GEditor.renderResolutionScale)),
        std::max(16u, static_cast<uint32_t>(displayExtent.y * GEditor.renderResolutionScale))
    };
    GEditor.camera.aspect = static_cast<float>(displayExtent.x) / static_cast<float>(displayExtent.y);
    GEditor.cameraUpdated |= GEditor.camera.UpdateMovement(dt);
    // An animated scene camera overrides user navigation while it's playing/scrubbing.
    if (followAnimatedCamera)
        GEditor.cameraUpdated |= ApplyAnimatedCameraToView();
    GEditor.camera.Update({});
    GEditor.shaderGlobals.frameNumber = renderer->GetFrame();
    GEditor.shaderGlobals.view = GEditor.camera.view;
    GEditor.shaderGlobals.proj = GEditor.camera.proj;
    GEditor.shaderGlobals.inverseView = inverse(GEditor.shaderGlobals.view);
    GEditor.shaderGlobals.inverseViewProj = inverse(GEditor.shaderGlobals.proj * GEditor.shaderGlobals.view);
    GEditor.shaderGlobals.zNear = GEditor.camera.zNear;
    GEditor.shaderGlobals.projPlanes = planeSymmetric(GEditor.shaderGlobals.proj);

    GEditor.shaderGlobals.camPosition = float4(GEditor.camera.position, 0);
    GEditor.shaderGlobals.camDirection = float4(GEditor.camera.rot * float3(0, 0, -1), 0);
    GEditor.shaderGlobals.aperture = GEditor.aperture.dofEnabled
        ? ApertureRadiusFromFStop(GEditor.aperture.fStop, GEditor.aperture.sensorHeightMm * 1e-3f,
                                  GEditor.camera.fovY)
        : 0.0f;
    // UBO fbWidth/fbHeight = internal render resolution (used by PT ray gen, raster lighting CS, etc.)
    GEditor.shaderGlobals.fbWidth = static_cast<float>(renderExtent.x);
    GEditor.shaderGlobals.fbHeight = static_cast<float>(renderExtent.y);
    GEditor.shaderGlobals.dbgViewFlags = GEditor.rendererConfig.viewFlags;
    GEditor.shaderGlobals.dbgMaterialFlags = GEditor.rendererConfig.materialFlags;
    GEditor.shaderGlobals.energyCompensation = GEditor.rendererConfig.energyCompensation ? 1u : 0u;

    // Skinned animation playback: wait for the scheduled poses, apply rigid transforms + CPU-skin
    // into the dynamic ring, then re-author the scene so dynamic instances encode the current ring
    // slot (the graph's BLAS Update pass refits them). Paused/held poses skip this so the path
    // tracer keeps accumulating.
    if (EndAnimationUpdate())
        CommitSceneToGPU(true);

    // -- AutoPause: any "user operation" exits AutoPaused. We define a user operation
    //    as anything that resets the path-tracer accumulation, which covers camera
    //    movement (cameraUpdated) and any UI control change that sets
    //    ptAccumulatedFrames = 0 (PT settings, light edits, etc.). A drop in
    //    ptAccumulatedFrames since the last frame is the canonical signal.
    static uint32_t sPrevPTAccumulatedFrames = 0u;
    bool ptAccumWasReset = GEditor.shaderGlobals.ptAccumulatedFrames < sPrevPTAccumulatedFrames;
    bool userOperation = GEditor.cameraUpdated || ptAccumWasReset;
    if (userOperation && GEditor.renderTask.renderAutoPaused)
    {
        GEditor.renderTask.renderAutoPaused = false;
        GEditor.renderTask.renderPaused = false;
    }
    if (GEditor.cameraUpdated)
        GEditor.shaderGlobals.ptAccumulatedFrames = 0, GEditor.cameraUpdated = false;
    RefreshPostprocessState(renderExtent);
    EditorGizmos::BuildLightGizmos();
    renderer->ExecuteFrame();
    renderer->EndExecute();
    // GPU picking: Blit PS wrote pickResult[0] this frame if a click was pending.
    if (sPickingPixel.x >= 0)
    {
        renderer->WaitForPreviousFrame();

        int const previousLight = GEditor.selectedLight;
        int const lightPick = EditorGizmos::PickLightAtRenderPixel(sPickingPixel);

        auto* pPickResultBuffer = renderer->DerefResource(sPickResultBuffer).Get<RHIBuffer*>();
        uint32_t id = *pPickResultBuffer->Map<uint32_t>();
        // The pick id is a TLAS instanceID, which equals the committed instance index
        // (the editor commits instances 1:1 in scene-row order).
        uint32_t pickedInstance = (id != ~0u && GContext->gpuScene)
            ? GContext->gpuScene->ResolvePickedInstance(id)
            : UINT32_MAX;

        auto selectLight = [&](int light)
        {
            GEditor.selectedLight = light;
            GEditor.selectedInstance = -1;
            GEditor.selectedMaterial = -1;
            GEditor.scrollSelectedLightToTop = true;
            GEditor.selectedLightHighlightStart = static_cast<float>(ImGui::GetTime());
        };
        auto selectInstance = [&](uint32_t instance)
        {
            GEditor.selectedInstance = static_cast<int>(instance);
            GEditor.selectedMaterial = static_cast<int>(GContext->gpuScene->GetInstance(instance).materialIndex);
            GEditor.selectedLight = -1;
        };
        auto clearSelection = [&]
        {
            GEditor.selectedLight = -1;
            GEditor.selectedInstance = -1;
            GEditor.selectedMaterial = -1;
        };

        bool const preferInstance = previousLight >= 0;
        bool const hasLight = lightPick >= 0;
        bool const hasInstance = pickedInstance != UINT32_MAX;
        bool pickedSelection = false;
        if (preferInstance)
        {
            if (hasInstance)
            {
                selectInstance(pickedInstance);
                pickedSelection = true;
            }
            else if (hasLight)
            {
                selectLight(lightPick);
                pickedSelection = true;
            }
            else
                clearSelection();
        }
        else
        {
            if (hasLight)
            {
                selectLight(lightPick);
                pickedSelection = true;
            }
            else if (hasInstance)
            {
                selectInstance(pickedInstance);
                pickedSelection = true;
            }
            else
                clearSelection();
        }
        if (sPickingDoubleClick && pickedSelection)
            GEditor.applySelectionDoubleClickAction = true;
        sPickingPixel = {-1, -1};
        sPickingDoubleClick = false;
    }
    if (!GEditor.renderTask.renderPaused)
        GEditor.shaderGlobals.ptAccumulatedFrames += GEditor.shaderGlobals.ptSamplesPerPixel;

    // If the sample limit is reached, auto-pause the render.
    if (GEditor.rendererMode == ERendererMode::PathTracer &&
        !GEditor.renderTask.renderPaused &&
        GEditor.renderTask.autoPauseSampleLimit > 0)
    {
        uint32_t completed = GEditor.shaderGlobals.ptAccumulatedFrames;
        if (completed >= static_cast<uint32_t>(GEditor.renderTask.autoPauseSampleLimit))
        {
            GEditor.renderTask.renderAutoPaused = true;
            GEditor.renderTask.renderPaused = true;
        }
    }
    sPrevPTAccumulatedFrames = GEditor.shaderGlobals.ptAccumulatedFrames;
}

static void FRenderingEnter() { 
    GEditor.rendererConfig.isRendering = true;
    FRunningEnter();  // Rebuild graph
    GEditor.state = FERendering;
}

static ImVec2 EventMousePosition(SDL_Event const& event)
{
    switch (event.type)
    {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return {event.button.x, event.button.y};
    case SDL_EVENT_MOUSE_MOTION:
        return {event.motion.x, event.motion.y};
    default:
        return ImGui::GetIO().MousePos;
    }
}

static bool ViewportAcceptsMouse(SDL_Event const& event)
{
    ImGuiIO& io = ImGui::GetIO();
    UpdateBackbufferViewport();
    ImVec2 pos = EventMousePosition(event);
    if (!GEditor.viewport.visible || !GEditor.viewport.Contains(pos))
        return false;
    return !io.WantCaptureMouse || !GEditor.showImGui;
}

static bool ViewportContainsMouse(SDL_Event const& event)
{
    UpdateBackbufferViewport();
    ImVec2 pos = EventMousePosition(event);
    return GEditor.viewport.visible && GEditor.viewport.Contains(pos);
}

bool EditorProcessEvent(SDL_Event* event)
{
    // Handle drag-and-drop file events
    if (event->type == SDL_EVENT_DROP_FILE)
    {
        const char* droppedFile = event->drop.data;
        if (droppedFile)
        {
            LOG(Editor, LogInfo, "File dropped: {}", droppedFile);
            HandleFile(droppedFile);
        }
    }
    bool windowOutputChanged = event->type == SDL_EVENT_WINDOW_RESIZED;
#if SDL_VERSION_ATLEAST(3, 2, 0)
    windowOutputChanged = windowOutputChanged || event->type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED;
#endif
    if (windowOutputChanged)
    {
        switch (GEditor.state)
        {
        case FENoScene:
            SetupIdleRenderer(GContext);
            break;
        case FERunning:
            GEditor.state = FERunningEnter;
            break;
        default:
            break;
        }
    }
    if (event->type == SDL_EVENT_KEY_DOWN)
    {
        if (event->key.key == SDLK_SPACE)
        {
            if (GEditor.camera.radius <= 1e-3f)
            {
                GEditor.camera.radius = std::max(length(GEditor.camera.position), 1.0f);
                vec3 dir = GEditor.camera.rot * vec3(0, 0, 1);
                GEditor.camera.center = GEditor.camera.position - dir * GEditor.camera.radius;
            }
            else
            {
                GEditor.camera.center = GEditor.camera.position;
                GEditor.camera.radius = 0.0f;
            }
            GEditor.cameraUpdated |= true;
        }
        if (event->key.key == SDLK_TAB)
        {
            GEditor.showImGui = !GEditor.showImGui;
            GEditor.shaderGlobals.dbgShowOutline = GEditor.showImGui;
        }
        // Gizmo hotkeys
        if (event->key.key == SDLK_G)
            GEditor.gizmo.op = ImGuizmo::TRANSLATE;
        if (event->key.key == SDLK_R)
            GEditor.gizmo.op = ImGuizmo::ROTATE;
        if (event->key.key == SDLK_Q)
            GEditor.gizmo.op = ImGuizmo::SCALE;
    }
    ImGui_ImplFoundation_ProcessEvent(event);
    auto& io = ImGui::GetIO();
    bool isRelative = SDL_GetWindowRelativeMouseMode(GContext->window);
    bool rightMouseEvent =
        (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_RIGHT) ||
        (event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_RIGHT) ||
        (event->type == SDL_EVENT_MOUSE_MOTION && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_RMASK));
    bool gizmoOver = ImGuizmo::IsOver();
    bool gizmoUsing = ImGuizmo::IsUsing();
    bool viewportMouse = ViewportAcceptsMouse(*event) ||
        (rightMouseEvent && gizmoOver && ViewportContainsMouse(*event));
    bool gizmoBlocksMouse = gizmoUsing || (gizmoOver && !rightMouseEvent);
    static bool sViewportRightDown = false;
    static bool sViewportRightDragged = false;
    static ImVec2 sViewportRightDownPos{};
    auto hasSelection = [] { return GEditor.selectedLight >= 0 || GEditor.selectedInstance >= 0; };
    if (isRelative || (viewportMouse && !gizmoBlocksMouse))
    {
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT)
            SDL_SetWindowRelativeMouseMode(GContext->window, true);
        GEditor.cameraUpdated |= GEditor.camera.Update(*event);
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_LEFT)
        SDL_SetWindowRelativeMouseMode(GContext->window, false);
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_RIGHT)
    {
        sViewportRightDown = viewportMouse && !gizmoUsing && hasSelection();
        sViewportRightDragged = false;
        sViewportRightDownPos = EventMousePosition(*event);
    }
    if (event->type == SDL_EVENT_MOUSE_MOTION && sViewportRightDown)
    {
        ImVec2 pos = EventMousePosition(*event);
        float dx = pos.x - sViewportRightDownPos.x;
        float dy = pos.y - sViewportRightDownPos.y;
        sViewportRightDragged |= dx * dx + dy * dy > 16.0f;
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_RIGHT)
    {
        if (sViewportRightDown && !sViewportRightDragged && viewportMouse && !gizmoUsing && hasSelection())
            GEditor.openSelectionContextMenu = true;
        sViewportRightDown = false;
    }
    // GPU picking: record click pixel on left mouse button release (not dragging)
    if (viewportMouse && !gizmoBlocksMouse)
    {
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT)
        {
            int2 pixel;
            if (GEditor.viewport.WindowPointToRenderPixel(EventMousePosition(*event), pixel))
            {
                sPickingPixel = pixel;
                sPickingDoubleClick = event->button.clicks >= 2;
            }
        }
    }
    // Always track WASD key state for the camera (only when ImGui does not need the keyboard)
    if (!io.WantCaptureKeyboard)
    {
        if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP)
        {
            bool pressed = (event->type == SDL_EVENT_KEY_DOWN);
            switch (event->key.key)
            {
            case SDLK_W: GEditor.camera.keyW = pressed; break;
            case SDLK_A: GEditor.camera.keyA = pressed; break;
            case SDLK_S: GEditor.camera.keyS = pressed; break;
            case SDLK_D: GEditor.camera.keyD = pressed; break;
            case SDLK_LSHIFT: case SDLK_RSHIFT: GEditor.camera.keyShift = pressed; break;
            default: break;
            }
        }
    }
    else
    {
        // When ImGui wants the keyboard, clear all movement key states to prevent stuck keys
        GEditor.camera.keyW = GEditor.camera.keyA = GEditor.camera.keyS = GEditor.camera.keyD = GEditor.camera.keyShift = false;
    }
    return false;
}

bool EditorOnFrame(FContext* context)
{
    ResetEditorFrameScratch(context);
    PumpDeferredSceneLoad();
    // Finalize/install any scene whose background upload finished this frame.
    PumpSceneLoad();
    switch (GEditor.state)
    {
    case FEInitEnter:
        FInitEnter();
        break;
    case FENoScene:
        FNoScene();
        break;
    case FERunningEnter:
        FRunningEnter();
        break;
    case FERunning:
        FRunning();
        break;
    case FERenderingEnter:
        FRenderingEnter();
        break;
    case FERendering:
        FRendering(sRenderOutputs);
        break;
    default:
        return true;
    }
    return false;
}

void EditorCleanup() { DestroyEditorRenderer(); }
