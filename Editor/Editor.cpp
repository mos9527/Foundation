#include "Editor.hpp"

#include "imgui_internal.h"
FEditorState FEState = FEInitEnter;
/* -- Scene Data -- */
static Vector<GSInstance> GSInstances(GLOBAL_ALLOC);
static UBO GShaderGlobals;
static FArcballCamera GCamera{
    .center = float3{0, 0, 0},
    .radius = 5.0f,
    .zNear = 0.1f,
    .fovY = radians(60.f),
};
/* -- */
List<Future<>> GUploadFutures(GLOBAL_ALLOC);
void FInitEnter()
{
    Vector<FMesh> meshes(GLOBAL_ALLOC);
    Vector<FInstance> instances(GLOBAL_ALLOC);
    Vector<FCamera> cameras(GLOBAL_ALLOC);
    CHECK_MSG(GContext->args.size() == 2, "Usage: Editor <scene path>");
    LoadFromFile(GContext->args[1], meshes, instances, cameras);
    for (auto& mesh : meshes)
    {
        LOG(Editor, LogInfo, "Loaded Mesh | Vtx={} | LODGroups={} | ApproxSize={} B", mesh.vertices.size(),
            mesh.dag.groups.size(), mesh.ApproximateSize());
    }
    if (cameras.size())
    {
        auto& camera = cameras.front();
        vec3 dir = camera.transform.rotation * vec3(0, 0, 1);
        GCamera.center = camera.transform.transform - dir * GCamera.radius;
        GCamera.rot = camera.transform.rotation;
        GCamera.fovY = camera.fovY;
    }
    // Load into GPUScene
    auto* scene = GContext->gpuScene;
    Vector<Pair<uint32_t, GSMesh>> meshOffsets(GLOBAL_ALLOC);
    for (auto& src : meshes)
    {
        auto& [offset, dst] = meshOffsets.emplace_back();
        auto& fut = GUploadFutures.emplace_back(scene->Upload(src, dst, offset));
        fut.wait(); // <- TODO: Somehow races. Revisit the whole 'streaming' idea.
    }
    GSInstances.clear();
    for (auto& src : instances)
    {
        auto& dst = GSInstances.emplace_back();
        dst.transform = src.transform.transform;
        dst.rotation = src.transform.rotation;
        dst.scale = src.transform.scale;
        dst.meshOffset = meshOffsets[src.meshIndex].first;
    }
    RendererSetupImGuiOnly(GContext);
    // ^^^ XXX: See below.
    FEState = FEInit;
}
void FInit()
{
    if (GUploadFutures.empty())
    {
        FEState = FERunningEnter;
        return;
    }
    // Collect upload progress
    for (auto it = GUploadFutures.begin(); it != GUploadFutures.end();)
    {
        if (it->wait_for(std::chrono::microseconds(0)) == std::future_status::ready)
            it->get(), it = GUploadFutures.erase(it);
    }
    auto* renderer = GContext->renderer;
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    ImGui::Begin("Editor");
    ImGui::Text("%ld remaining", GUploadFutures.size());
    ImGui::End();
    renderer->ExecuteFrame();
    renderer->EndExecute();
    FEState = FEInit;
}
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
    auto [ptr, off] = scene->InstanceAlloc(GSInstances.size());
    std::memcpy(ptr, GSInstances.data(), GSInstances.size() * sizeof(GSInstance));
    GShaderGlobals.firstInstance = off;
    GShaderGlobals.numInstances = GSInstances.size();
    // Global param update
    GCamera.Update({});
    GCamera.aspect = GContext->swapchain->GetAspectRatio();
    GShaderGlobals.view = GCamera.view;
    GShaderGlobals.proj = GCamera.proj;
    GShaderGlobals.zNear = GCamera.zNear;
    GShaderGlobals.projPlanes = planeSymmetric(GShaderGlobals.proj);
    // ImGui
    if (ImGui::Begin("Debug"))
    {
        ImGui::TextUnformatted(FArcballCamera::kControlsText);
        ImGui::Text("FPS | %.2f", ImGui::GetIO().Framerate);
        ImGui::SliderFloat("LOD Threshold | ", &GShaderGlobals.lodThreshold, 0.f, 1.f);
        ImGui::Separator();
        ImGui::SliderFloat3("Cam Center", &GCamera.center.x, -50.0f, 50.0f);
        ImGui::SliderFloat("Cam Radius", &GCamera.radius, 0.0f, 100.0f);
        ImGui::SliderAngle("Cam FOV Y", &GCamera.fovY);
    }
    ImGui::End();
    if (ImGui::Begin("Rendering"))
    {
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
                    ImGui::Text("CPU Memory Usage: %.2f MB", used / 1e6f);
                }
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("GPU Scene", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("Streaming: %s", GContext->gpuScene->DbgGetStreamingStatistics().c_str());
                ImGui::Text("Buffers:   %s", GContext->gpuScene->DbgGetBufferStatistics().c_str());
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Frametime", ImGuiTreeNodeFlags_DefaultOpen))
            {
                static Vector<ImProfilerSample> samples(GLOBAL_ALLOC);
                static bool pause = false;
                static float presentTimingMS = 0.0f;
                static float gpuTimingMS = 0.0f;
                static int lanes = 0;
                static float frametime = 0;
                ImGui::Text("CPU to Present: %.3fms, Present to Present %.3fms, GPU: %.3fms, CPU/GPU Δ: %.3fms", presentTimingMS, frametime, gpuTimingMS, frametime - gpuTimingMS);
                if (ImModalButton(pause ? "Resume" : "Pause"))
                    pause = !pause;
                if (!pause)
                {
                    samples.clear();
                    for (size_t i = 0; i < timings.size() / 2; i++)
                    {
                        auto const& pass = renderer->GetTrackedPass(i);
                        ImProfilerSample sample{
                            .startTick = timings[i * 2],
                            .endTick = timings[i * 2 + 1],
                            .label = pass.name,
                            .color = pass.queue == RHIDeviceQueueType::Graphics ? ImColor(1.0f, 0.5f, 0.0f, 1.0f)
                                                                                : ImColor(0.0f, 0.5f, 0.0f, 1.0f),
                        };
                        samples.emplace_back(std::move(sample));
                    }
                    float presentTimingRes;
                    lanes = ImProfilerAssignLanes(samples);
                    gpuTimingMS = (samples.back().endTick - samples.front().startTick) * 1e-6;
                    gpuTimingMS *= gpuTimingRes;
                    presentTimingMS = renderer->DbgProfilePresentTiming(renderer->GetSync(), presentTimingRes) * 1e-6;
                    presentTimingMS *= presentTimingRes;
                    frametime = ImGui::GetIO().DeltaTime * 1e3f;
                }
                ImGui::SeparatorText("Timeline");
                ImProfilerDrawTimestampLabel(samples, gpuTimingRes, 8u);
                for (int lane = 0; lane < lanes; lane++)
                    ImProfilerDrawLane(samples, gpuTimingRes, lane);
                if (ImGui::TreeNode("Tables"))
                {
                    for (int lane = 0; lane < lanes; lane++)
                        ImProfilerDrawTable(samples, gpuTimingRes, lane);
                    ImGui::TreePop();
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
