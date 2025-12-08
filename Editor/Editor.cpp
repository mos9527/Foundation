#include "Editor.hpp"

FEditorState FEState = FEInitEnter;
/* -- Scene Data -- */
static Vector<GSInstance> GSInstances(GLOBAL_ALLOC);
static Vector<GSMaterial> GSMaterials(GLOBAL_ALLOC);
static UBO GShaderGlobals;
static FArcballCamera GCamera{
    .center = float3{0, 0, 0},
    .radius = 5.0f,
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
    // Load into GPUScene
    auto* gpu = GContext->gpuScene;
    Vector<Pair<uint32_t, GSMesh>> meshOffsets(GLOBAL_ALLOC);
    Vector<uint32_t> textureIDMap(scene.mTextures.size(), GLOBAL_ALLOC);
    LOG(Editor, LogInfo, "Uploading scene to GPU");
    {
        ImmediateUpload upload(GContext->device.Get(), 128 * (1u << 20)); // MB
        upload.Begin();
        for (auto& src : scene.mMeshes)
        {
            CHECK(src.EnsureQuantized());
            auto& [offset, dst] = meshOffsets.emplace_back();
            if (!gpu->Upload(&upload, src, dst, offset))
            {
                // Flush batched uploads - staging buffer full
                upload.End(), upload.WaitIdle(), upload.Begin();
                CHECK_MSG(gpu->Upload(&upload, src, dst, offset), "Staging buffer too small for single mesh upload");
            }
        }
        for (int id = 0; auto& src : scene.mTextures)
        {
            if (!gpu->Upload(&upload, src, textureIDMap[id]))
            {
                upload.End(), upload.WaitIdle(), upload.Begin();
                CHECK_MSG(gpu->Upload(&upload, src, textureIDMap[id]),
                          "Staging buffer too small for single texture upload");
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
        dst.meshOffset = meshOffsets[src.meshIndex].first;
        dst.materialIndex = src.materialIndex;
    }
    GSMaterials.clear();
    for (auto& src : scene.mMaterials)
    {
        auto& dst = GSMaterials.emplace_back();
        dst.baseColorFactor = src.baseColorFactor;
        dst.emissiveFactor = src.emissiveFactor;
        dst.metallicFactor = src.metallicFactor;
        dst.roughnessFactor = src.roughnessFactor;
        dst.baseColorTexture = textureIDMap[src.baseColorTexture];
        dst.emissiveTexture = textureIDMap[src.baseColorTexture];
        dst.metallicRoughnessTexture = textureIDMap[src.metallicRoughnessTexture];
        dst.normalTexture = textureIDMap[src.normalTexture];
    }
    FEState = FEInit;
}
void FInit() { FEState = FERunningEnter; }
RendererConfig GRendererConfig;
void FRunningEnter()
{
    RendererSetup(GContext, &GShaderGlobals, GRendererConfig);
    FEState = FERunning;
}
void FRunning()
{
    auto* renderer = GContext->renderer;
    auto* scene = GContext->gpuScene;
    // New frame
    renderer->BeginExecute();
    float gpuTimingRes;
    auto timings = renderer->DbgProfilePassTiming(renderer->GetSync(), gpuTimingRes);
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    // Upload instance data
    {
        auto [ptr, off] = scene->AllocateInstance(GSInstances.size());
        std::memcpy(ptr, GSInstances.data(), GSInstances.size() * sizeof(GSInstance));
        GShaderGlobals.firstInstance = off;
        GShaderGlobals.numInstances = GSInstances.size();
    }
    // Upload material data
    {
        auto [ptr, off] = scene->AllocateMaterial(GSMaterials.size());
        std::memcpy(ptr, GSMaterials.data(), GSMaterials.size() * sizeof(GSMaterial));
        GShaderGlobals.firstMaterial = off;
        GShaderGlobals.numMaterials = GSMaterials.size();
    }
    // Global param update
    GCamera.Update({});
    GCamera.aspect = GContext->swapchain->GetAspectRatio();
    GShaderGlobals.view = GCamera.view;
    GShaderGlobals.proj = GCamera.proj;
    GShaderGlobals.zNear = GCamera.zNear;
    GShaderGlobals.projPlanes = planeSymmetric(GShaderGlobals.proj);
    // ImGui
    if (ImGui::Begin("Camera"))
    {
        ImGui::TextUnformatted(FArcballCamera::kControlsText);
        ImGui::Separator();
        ImGui::SliderFloat3("Cam Center", &GCamera.center.x, -50.0f, 50.0f);
        ImGui::SliderFloat("Cam Radius", &GCamera.radius, 0.0f, 100.0f);
        ImGui::SliderAngle("Cam FOV Y", &GCamera.fovY);
    }
    ImGui::End();
    if (ImGui::Begin("Rendering"))
    {
        static float lodLogThreshold = 2;
        ImGui::SliderFloat("LOD Threshold | ", &lodLogThreshold, 0, 8);
        GShaderGlobals.lodThreshold = std::pow(10.0f, -lodLogThreshold);
        if (ImGui::Button("Toggle Overdraw View"))
        {
            GRendererConfig.viewFlags ^= kViewOverdraw;
            FEState = FERunningEnter;
        }
        if (ImGui::Button("Toggle Meshlet View"))
        {
            GRendererConfig.viewFlags ^= kViewMeshlet;
            FEState = FERunningEnter;
        }
        if (ImGui::Button("Toggle HIZ View"))
        {
            GRendererConfig.viewFlags ^= kViewHIZ;
            FEState = FERunningEnter;
        }
        if (ImGui::Button("Toggle Frustum Culling"))
        {
            GRendererConfig.cullFlags ^= kCullFrustum;
            FEState = FERunningEnter;
        }
        if (ImGui::Button("Toggle Occlusion Culling"))
        {
            GRendererConfig.cullFlags ^= kCullOcclusion;
            FEState = FERunningEnter;
        }
        if (ImGui::Button("Toggle GBuffer BaseColor"))
        {
            GRendererConfig.gbufferFlags ^= kGBufferViewBaseColor;
            FEState = FERunningEnter;
        }
        if (ImGui::Button("Toggle GBuffer Normal"))
        {
            GRendererConfig.gbufferFlags ^= kGBufferViewNormal;
            FEState = FERunningEnter;
        }
        if (ImGui::Button("Toggle GBuffer MaterialID"))
        {
            GRendererConfig.gbufferFlags ^= kGBufferViewMaterialID;
            FEState = FERunningEnter;
        }
        if (ImGui::Button("Reload"))
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
                static constexpr size_t kHistogramSamples = 5e3, kFrametimeSamples = 1e3;
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
                            .color = pass.queue == RHIDeviceQueueType::Graphics ? ImColor(1.0f, 0.5f, 0.0f, 1.0f)
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
    renderer->ExecuteFrame();
    renderer->EndExecute();
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
    ImGui_ImplFoundation_ProcessEvent(event);
    auto& io = ImGui::GetIO();
    if (!io.WantCaptureMouse)
        GCamera.Update(*event);
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
