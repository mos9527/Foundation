#include "Editor.hpp"
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
        GUploadFutures.emplace_back(scene->Upload(src, dst, offset));
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
    float timingResolutionNS;
    auto timings = renderer->GetPassTimings(renderer->GetSync(), timingResolutionNS);
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
        ImGui::Text("StreamingPool | %s", scene->DbgGetStatistics().c_str());
        ImGui::Text("Instances | %zu", GSInstances.size());
        ImGui::SliderFloat("LOD Threshold | ", &GShaderGlobals.lodThreshold, 0.f, 1.f);
        ImGui::Separator();
        ImGui::SliderFloat3("Cam Center", &GCamera.center.x, -50.0f, 50.0f);
        ImGui::SliderFloat("Cam Radius", &GCamera.radius,0.0f, 100.0f);
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
    if (ImGui::Begin("Profiling"))
    {
        if (timings.empty())
        {
            ImGui::Text("No Info");
        } else
        {
            Vector<Tuple<size_t,size_t,PassHandle, int>> sorted(GLOBAL_ALLOC);
            for (size_t i = 0; i < timings.size() / 2; i++)
                sorted.emplace_back(timings[i * 2],timings[i * 2 + 1], i, 0);
            std::sort(sorted.begin(), sorted.end());
            // Partition into ranks - work has chance to overlap on the GPU.
            int top = 0; PriorityQueue<Pair<size_t, int>, std::greater<>> Q(GLOBAL_ALLOC);
            for (auto& [start, end, pass, rank] : sorted)
            {
                if (!Q.empty() && Q.top().first <= start)
                {
                    rank = Q.top().second; Q.pop();
                    Q.emplace(end, rank);
                }
                else
                {
                    rank = top++;
                    Q.emplace(end, rank);
                }
            }
            Vector<float> rankTicks(top, GLOBAL_ALLOC);
            ImGui::Text("Pass Timings (ns):");
            size_t begin = std::get<0>(sorted.front());
            for (auto& [start, end, pass, rank] : sorted)
                start -= begin, end -= begin, rankTicks[rank] += end - start;
            for (auto& [start, end, pass, rank] : sorted)
            {
                float duration = end - start;
                float rankP = duration / rankTicks[rank];
                ImGui::Text("%s | %.2fms | %.2f%% of Rank Time", renderer->GetTrackedPass(pass).name.c_str(), duration * timingResolutionNS * 1e-6f, rankP * 100.f);
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
