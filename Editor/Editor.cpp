#include "Editor.hpp"

FEditorState FEState = FEInitEnter;
/* -- Scene Data -- */
static Vector<GSInstance> GSInstances(GLOBAL_ALLOC);
static Vector<GSMaterial> GSMaterials(GLOBAL_ALLOC);
static Vector<GSMesh> GSMeshes(GLOBAL_ALLOC);
static Vector<uint32_t> GSBLASes(GLOBAL_ALLOC);
/* -- Camera & Globals -- */
static UBO GShaderGlobals;
static FArcballCamera GCamera{
    .center = float3{0, 0, 0},
    .radius = 1.0f,
    .zNear = 0.1f,
    .fovY = radians(60.f),
};
/* -- */
void FInitEnter()
{
    FScene scene(GLOBAL_ALLOC);
    CHECK_MSG(GContext->args.size() == 2, "Usage: Editor <scene path>");
    LoadScene(GContext->args[1], scene);
    if (!scene.mCameras.empty())
    {
        auto& camera = scene.mCameras.front();
        vec3 dir = camera.transform.rotation * vec3(0, 0, 1);
        GCamera.center = camera.transform.transform - dir * GCamera.radius;
        GCamera.rot = camera.transform.rotation;
        GCamera.fovY = camera.fovY;
    }
    if (!scene.mLights.empty())
    {
        auto& light = scene.mLights.front();
        GShaderGlobals.sunDirection = normalize(light.transform.rotation * float3(0, 0, -1));
        GShaderGlobals.sunIntensity = light.color * light.intensity;
        GShaderGlobals.camMinEV = 0.0f;
        GShaderGlobals.camMaxEV = log2f(light.intensity * 0.25f);
    }
    else
    {
        GShaderGlobals.sunIntensity = {};
    }
    // Load into GPUScene
    auto* gpu = GContext->gpuScene;
    Vector<uint32_t> meshOffsets(GLOBAL_ALLOC);
    Vector<uint32_t> textureIDMap(scene.mTextures.size(), GLOBAL_ALLOC);
    LOG(Editor, LogInfo, "Uploading scene to GPU");
    GSMeshes.clear();
    {
        ImmediateUpload upload(GContext->device.Get(), 128 * (1u << 20)); // MB
        upload.Begin();
        for (auto& src : scene.mMeshes)
        {
            CHECK(src.EnsureQuantized());
            CHECK(src.EnsureRaw());
            auto& dst = GSMeshes.emplace_back();
            auto& offset = meshOffsets.emplace_back();
            if (!gpu->Upload(&upload, src, dst, offset))
            {
                // Flush batched uploads - staging buffer full
                upload.End(), upload.WaitIdle(), upload.Begin();
                CHECK_MSG(gpu->Upload(&upload, src, dst, offset), "Staging buffer too small for single mesh upload");
            }
        }
        for (int id = 0; auto& src : scene.mTextures)
        {
            if (!src.IsValid())
            {
                textureIDMap[id] = 0; // Default texture
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
    GSInstances.clear();
    for (auto& src : scene.mInstances)
    {
        auto& dst = GSInstances.emplace_back();
        dst.transform = src.transform.transform;
        dst.rotation = src.transform.rotation;
        dst.scale = src.transform.scale;
        dst.meshOffset = meshOffsets[src.meshIndex];
        dst.materialIndex = src.materialIndex;
        dst.meshIndex = src.meshIndex;
    }
    GSMaterials.clear();
    for (auto& src : scene.mMaterials)
    {
        auto& dst = GSMaterials.emplace_back();
        dst.baseColorFactor = src.baseColorFactor;
        dst.emissiveFactor = src.emissiveFactor;
        dst.metallicFactor = src.metallicFactor;
        dst.roughnessFactor = src.roughnessFactor;
        dst.baseColorTexture = src.baseColorTexture ? textureIDMap[src.baseColorTexture] : 0u;
        dst.emissiveTexture = src.emissiveTexture ? textureIDMap[src.emissiveTexture] : 0u;
        dst.metallicRoughnessTexture = src.metallicRoughnessTexture ? textureIDMap[src.metallicRoughnessTexture] : 0u;
        dst.normalTexture = src.normalTexture ? textureIDMap[src.normalTexture] : 0u;
        dst.transmissionFactor = src.transmissionFactor;
    }
    // Upload instance data
    {
        auto [ptr, off] = gpu->AllocateInstance(GSInstances.size());
        std::memcpy(ptr, GSInstances.data(), GSInstances.size() * sizeof(GSInstance));
        GShaderGlobals.firstInstance = off;
        GShaderGlobals.numInstances = GSInstances.size();
    }
    // Upload material data
    {
        auto [ptr, off] = gpu->AllocateMaterial(GSMaterials.size());
        std::memcpy(ptr, GSMaterials.data(), GSMaterials.size() * sizeof(GSMaterial));
        GShaderGlobals.firstMaterial = off;
        GShaderGlobals.numMaterials = GSMaterials.size();
    }
    // Build RT AS
    {
        ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
        GSBLASes.resize(GSMeshes.size());
        LOG(Editor, LogDebug, "Building BLAS");
        constexpr size_t kBLASBuildBatch = 32u;
        for (size_t i = 0; i < GSMeshes.size(); i += kBLASBuildBatch)
        {
            Span<GSMesh> meshesBatch = GSMeshes;
            Span<uint32_t> indicesBatch = GSBLASes;
            size_t batchSize = std::min(kBLASBuildBatch, GSMeshes.size() - i);
            meshesBatch = meshesBatch.subspan(i, batchSize);
            indicesBatch = indicesBatch.subspan(i, batchSize);
            LOG(Editor, LogDebug, "Building BLAS {} to {}", i, i + batchSize);
            gpu->BuildBLAS(&ctx, meshesBatch, indicesBatch);
        }
        LOG(Editor, LogDebug, "Building TLAS");
        ctx->Begin();
        gpu->BuildTLAS(ctx.Get(), GSInstances, GSBLASes, false);
        ctx->End(), ctx.Submit(), ctx.WaitIdle();
    }
    FEState = FEInit;
}

void FInit() { FEState = FERunningEnter; }
RendererConfig GRendererConfig;

static bool rasterOrPT = true;
void FRunningEnter()
{
    RendererScene scene{
        .gsGlobals = &GShaderGlobals,
        .gsInstances = &GSInstances,
        .gsMaterials = &GSMaterials,
        .gsMeshes = &GSMeshes,
        .gsBLASes = &GSBLASes
    };
    if (rasterOrPT)
        PathTracerSetup(GContext, GRendererConfig, scene);
    else
        RendererSetup(GContext, GRendererConfig, scene);
    FEState = FERunning;
}

bool cameraUpdated = true;

void FRunningImGui()
{
    auto* renderer = GContext->renderer;
    float gpuTimingRes;
    auto timings = renderer->DbgProfilePassTiming(renderer->GetSync(), gpuTimingRes);
    // ImGui
    if (ImGui::Begin("Camera"))
    {
        ImGui::TextUnformatted(FArcballCamera::kControlsText);
        ImGui::Separator();
        cameraUpdated |= ImGui::SliderFloat3("Cam Center", &GCamera.center.x, -50.0f, 50.0f);
        cameraUpdated |= ImGui::SliderFloat("Cam Radius", &GCamera.radius, 0.0f, 100.0f);
        cameraUpdated |= ImGui::SliderAngle("Cam FOV Y", &GCamera.fovY);
        cameraUpdated |= ImGui::SliderFloat("Aperture", &GShaderGlobals.aperture, 1e-5f, 1.0f, "%.5f", ImGuiSliderFlags_Logarithmic);
        cameraUpdated |= ImGui::SliderFloat("Focal Distance", &GShaderGlobals.focalDistance, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Min EV", &GShaderGlobals.camMinEV, -16.0f, 16.0f);
        ImGui::SliderFloat("Max EV", &GShaderGlobals.camMaxEV, -16.0f, 16.0f);
        ImGui::SliderFloat("Adapt Rate", &GCamera.adaptRate, 0.0f, 100.0f);
    }
    ImGui::End();
    if (ImGui::Begin("Rendering"))
    {
        static float lodLogThreshold = 3;
        ImGui::SliderFloat("LOD ", &lodLogThreshold, 0, 8);
        if (rasterOrPT)
        {
            ImGui::Text("PT Accumulation: %d", GShaderGlobals.ptAccumualatedFrames);
            ImGui::SliderInt("PT Bounces", &GShaderGlobals.ptMaxBounces, 1, 16);
        }
        GShaderGlobals.lodThreshold = std::pow(10.0f, -lodLogThreshold);
        bool changed = false;
        {
            const char* items[] = {"Overdraw", "Meshlet", "Material ID"};
            const unsigned values[] = {kViewOverdraw, kViewMeshlet, kViewMaterialID};
            ImGui::SeparatorText("Perf Debug View");
            changed |= ImBitmaskOptionPicker(GRendererConfig.viewFlags, items, values, true /* solo */);
        }
        {
            const char* items[] = {"Position", "BaseColor", "Normal"};
            const unsigned values[] = {kViewPosition, kViewBaseColor, kViewNormal};
            ImGui::SeparatorText("GBuffer View");
            changed |= ImBitmaskOptionPicker(GRendererConfig.viewFlags, items, values, true /* solo */);
        }
        {
            const char* items[] = {"PT Direct Only"};
            const unsigned values[] = {kViewPTDirect};
            ImGui::SeparatorText("Path Tracer View");
            changed |= ImBitmaskOptionPicker(GRendererConfig.viewFlags, items, values, true /* solo */);
        }
        {
            const char* items[] = {"Frustum", "Occlusion"};
            const unsigned values[] = {kCullFrustum, kCullOcclusion};
            ImGui::SeparatorText("Culling");
            changed |= ImBitmaskOptionPicker(GRendererConfig.cullFlags, items, values);
        }
        changed |= ImGui::Button("Reload");
        if (changed)
            FEState = FERunningEnter;
    }
    ImGui::End();
    if (ImGui::Begin("Profiler"))
    {
        if (timings.empty())
        {
            ImGui::Text("No Info");
        }
        else
        {
            if (ImGui::TreeNodeEx("Device", ImGuiTreeNodeFlags_DefaultOpen))
            {
                {
                    size_t used, budget;
                    GContext->device->QueryBudget(used, budget);
                    static String name;
                    if (name.empty())
                        name = GContext->device->QueryDeviceString();
                    ImGui::Text("%s", name.c_str());
                    ImGui::Text("GPU Memory Usage: %.2f MB / %.2f MB", used / 1e6f, budget / 1e6f);
                }
                {
                    size_t used, budget;
                    GLOBAL_ALLOC->QueryBudget(used, budget);
                    ImGui::Text("CPU RSS Memory: %.2f MB", used / 1e6f);
                }
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("GPU Scene", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("%s", GContext->gpuScene->DbgGetBufferStatistics().c_str());
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Frametime", ImGuiTreeNodeFlags_DefaultOpen))
            {
                static constexpr size_t kHistogramSamples = 5e3, kFrametimeSamples = 3e2;
                static Vector<ImProfilerSample> samples(GLOBAL_ALLOC);
                static Vector<ImProfilerHistogram> histograms(GLOBAL_ALLOC);
                static ImProfilerHistogram frametime(kFrametimeSamples, GLOBAL_ALLOC);
                static bool pause = false;
                static float presentTimingMS = 0.0f;
                static float gpuTimingMS = 0.0f;
                static int lanes = 0;
                float frametimeAvg = frametime.mean * 1e-6f;
                ImGui::Text("CPU to Present: %.3fms, Present to Present Rolling Avg.: %.3fms (%.1f FPS), GPU: %.3fms, "
                            "CPU/GPU Δ: %.3fms",
                            presentTimingMS, frametimeAvg * 1e3f, 1 / frametimeAvg, gpuTimingMS,
                            frametimeAvg * 1e3f - gpuTimingMS);
                auto ClearHistogramData = []
                {
                    for (auto& hist : histograms)
                        hist.clear();
                };
                if (ImModalButton(pause ? "Resume" : "Pause", 0, 2))
                    pause = !pause;
                if (ImModalButton("Flush", 1, 2))
                    ClearHistogramData();
                if (!pause)
                {
                    samples.clear();
                    for (size_t i = 0; i < timings.size() / 2; i++)
                    {
                        auto const& pass = renderer->GetTrackedPass(i);
                        ImProfilerSample sample{
                            .id = static_cast<int>(i),
                            .startTick = timings[i * 2],
                            .endTick = timings[i * 2 + 1],
                            .label = pass.name,
                            .color = pass.queue == RHIDeviceQueueType::Graphics
                            ? ImColor(1.0f, 0.5f, 0.0f, 1.0f)
                            : ImColor(0.0f, 0.5f, 0.0f, 1.0f),
                        };
                        samples.emplace_back(std::move(sample));
                        while (histograms.size() <= i)
                            histograms.emplace_back(kHistogramSamples, GLOBAL_ALLOC);
                        histograms[i].push(sample.endTick - sample.startTick);
                    }
                    float presentTimingRes;
                    lanes = ImProfilerAssignLanes(samples);
                    gpuTimingMS = (samples.back().endTick - samples.front().startTick) * 1e-6;
                    gpuTimingMS *= gpuTimingRes;
                    presentTimingMS = renderer->DbgProfilePresentTiming(renderer->GetSync(), presentTimingRes) * 1e-6;
                    presentTimingMS *= presentTimingRes;
                    frametime.push(ImGui::GetIO().DeltaTime * 1e6f);
                }
                int selectedID = -1;
                static int maxLanes = 0;
                ImProfilerDrawTimestampLabel(samples, gpuTimingRes, 8u);
                for (int lane = 0; lane < std::max(maxLanes, lanes); lane++)
                    selectedID = std::max(selectedID, ImProfilerDrawLane(samples, lane));
                maxLanes = std::max(maxLanes, lanes);
                if (ImGui::TreeNode("Tables"))
                {
                    Ranges::sort(samples, [](auto const& a, auto const& b) { return a.id < b.id; });
                    selectedID = std::max(selectedID, ImProfilerDrawTable(samples, gpuTimingRes));
                    ImGui::TreePop();
                }
                if (selectedID >= 0)
                {
                    ImGui::SetNextWindowSize({ImGui::GetWindowSize().x * 0.75f, 0});
                    if (ImGui::BeginTooltip())
                    {
                        if (!pause)
                            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                                               "Profiler still running. Please Pause for accurate Histogram data.");
                        auto it = Ranges::find_if(samples,
                                                  [selectedID](auto const& sample) { return sample.id == selectedID; });
                        ImGui::SeparatorText(it->label.c_str());
                        if (it != samples.end())
                        {
                            auto& hist = histograms[selectedID];
                            Vector<unsigned> bins(GLOBAL_ALLOC);
                            hist.bin(bins, 256, false /* log */);
                            float mean = hist.mean * gpuTimingRes * 1e-6f,
                                  median = hist.sorted[hist.sorted.size() / 2] * gpuTimingRes * 1e-6f,
                                  stddev = hist.stddev() * gpuTimingRes * 1e-6f;
                            ImGui::Text("Mean: %.3fms | Median: %.3fms | σ: %.3fms", mean, median, stddev);
                            ImProfilerDrawHistogram(bins, hist, 8, gpuTimingRes, false /* log */);
                        }
                        ImGui::EndTooltip();
                    }
                }
                ImGui::TreePop();
            }
        }
    }
    ImGui::End();
}
static bool enableImGui = false;
void FRunning()
{
    auto* renderer = GContext->renderer;
    // New frame
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    if (enableImGui)
        FRunningImGui();
    // Global param update
    GCamera.Update({});
    GCamera.aspect = GContext->swapchain->GetAspectRatio();
    GShaderGlobals.frameNumber = renderer->GetFrame();
    GShaderGlobals.view = GCamera.view;
    GShaderGlobals.proj = GCamera.proj;
    GShaderGlobals.inverseView = inverse(GShaderGlobals.view);
    GShaderGlobals.inverseViewProj = inverse(GShaderGlobals.proj * GShaderGlobals.view);
    GShaderGlobals.zNear = GCamera.zNear;
    GShaderGlobals.projPlanes = planeSymmetric(GShaderGlobals.proj);
    static float prevTime = 0.0f;
    float deltaTime = SDL_GetTicks() - prevTime;
    prevTime = SDL_GetTicks();
    GShaderGlobals.camAdaptCoeff = 1.0f - std::exp(-deltaTime * GCamera.adaptRate);
    GShaderGlobals.camPosition = GCamera.position;
    GShaderGlobals.camDirection = GCamera.rot * float3(0, 0, -1);
    GShaderGlobals.fbWidth = static_cast<float>(renderer->GetSwapchainExtent().x);
    GShaderGlobals.fbHeight = static_cast<float>(renderer->GetSwapchainExtent().y);
    if (cameraUpdated)
        GShaderGlobals.ptAccumualatedFrames = 0, cameraUpdated = false;
    renderer->ExecuteFrame();
    renderer->EndExecute();
    GShaderGlobals.ptAccumualatedFrames++;
}

bool EditorProcessEvent(SDL_Event* event)
{
    if (event->type == SDL_EVENT_WINDOW_RESIZED)
    {
        switch (FEState)
        {
        case FEInit:
            RendererSetupImGuiOnly(GContext);
            break;
        case FERunning:
            FEState = FERunningEnter;
            break;
        default:
            break;
        }
    }
    if (event->type == SDL_EVENT_KEY_DOWN)
    {
        if (event->key.key == SDLK_SPACE)
        {
            GCamera.radius = std::max(length(GCamera.center), 1.0f);
            GCamera.center = {};
            cameraUpdated |= true;
        }
        if (event->key.key == SDLK_TAB)
        {
            enableImGui = !enableImGui;
        }
        if (event->key.key == SDLK_R)
        {
            rasterOrPT = !rasterOrPT;
            FEState = FERunningEnter;
        }
    }
    ImGui_ImplFoundation_ProcessEvent(event);
    auto& io = ImGui::GetIO();
    if (!io.WantCaptureMouse)
        cameraUpdated |= GCamera.Update(*event);
    return false;
}

// Per-frame logic
bool EditorOnFrame(FContext*)
{
    switch (FEState)
    {
    case FEInitEnter:
        FInitEnter();
        break;
    case FEInit:
        FInit();
        break;
    case FERunningEnter:
        FRunningEnter();
        break;
    case FERunning:
        FRunning();
        break;
    default:
        return true;
    }
    return false;
}
