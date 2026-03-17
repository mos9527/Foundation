#include "Editor.hpp"
#include "FileDialog.hpp"
#include <imgui_internal.h>

FEditorState FEState = FEInitEnter;
/* -- Scene Data -- */
static Vector<GSInstance> GSInstances(GLOBAL_ALLOC);
static Vector<GSMaterial> GSMaterials(GLOBAL_ALLOC);
static Vector<GSMesh> GSMeshes(GLOBAL_ALLOC);
static Vector<uint32_t> GSBLASes(GLOBAL_ALLOC);
/* -- Persistent CPU Scene (Task 3) -- */
static FScene GPersistentScene(GLOBAL_ALLOC);
static String GCurrentSavePath;
static int GSelectedInstance = -1;
static bool GShowImGui = true;
/* -- Camera & Globals -- */
static UBO GShaderGlobals;
static FArcballCamera GCamera{
    .center = float3{0, 0, 0},
    .radius = 1.0f,
    .zNear = 0.1f,
    .fovY = radians(60.f),
};

/* -- 前向声明 -- */
static void ReplaceScene(StringView path);
static void SaveScene(StringView path);
static void LoadEnvMap(StringView path);
static void EditorDockSpaceAndMenuBar();
static void FHierarchyPanel();

/* -- */
void FInitEnter()
{
    // Task 2: 支持无CLI参数启动
    if (GContext->args.size() < 2)
    {
        LOG(Editor, LogInfo, "No scene path provided, starting with empty scene");
        RendererSetupImGuiOnly(GContext);
        FEState = FEInit;
        return;
    }
    ReplaceScene(GContext->args[1]);
    if (FEState != FERunningEnter)
        FEState = FEInit;
}

/* ==================== ReplaceScene ==================== */
static void ReplaceScene(StringView path)
{
    LOG(Editor, LogInfo, "Loading scene: {}", path);
    FScene newScene(GLOBAL_ALLOC);
    try
    {
        LoadScene(path, newScene);
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to load scene: {}", path);
        return;
    }

    // 清空编辑器侧数据
    GSInstances.clear();
    GSMaterials.clear();
    GSMeshes.clear();
    GSBLASes.clear();
    GPersistentScene = FScene(GLOBAL_ALLOC);
    GSelectedInstance = -1;

    auto* gpu = GContext->gpuScene;
    gpu->Reset();

    Vector<uint32_t> meshOffsets(GLOBAL_ALLOC);
    Vector<uint32_t> textureIDMap(newScene.mTextures.size(), GLOBAL_ALLOC);

    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    // 上传Mesh和Texture
    {
        ImmediateUpload upload(GContext->device.Get(), 128 * (1u << 20));
        upload.Begin();
        for (auto& src : newScene.mMeshes)
        {
            CHECK(src.EnsureQuantized());
            CHECK(src.EnsureRaw());
            auto& dst = GSMeshes.emplace_back();
            auto& offset = meshOffsets.emplace_back();
            if (!gpu->Upload(&upload, src, dst, offset))
            {
                upload.End(), upload.WaitIdle(), upload.Begin();
                CHECK_MSG(gpu->Upload(&upload, src, dst, offset), "Staging buffer too small for single mesh upload");
            }
        }
        for (int id = 0; auto& src : newScene.mTextures)
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

    // 移动到持久场景
    for (auto& m : newScene.mMeshes)
        GPersistentScene.mMeshes.emplace_back(std::move(m));
    for (auto& t : newScene.mTextures)
        GPersistentScene.mTextures.emplace_back(std::move(t));

    // 材质：映射纹理索引
    for (auto& src : newScene.mMaterials)
    {
        GPersistentScene.mMaterials.emplace_back(src);

        auto& dst = GSMaterials.emplace_back();
        dst.baseColorFactor = src.baseColorFactor;
        dst.emissiveFactor = src.emissiveFactor;
        dst.metallicFactor = src.metallicFactor;
        dst.roughnessFactor = src.roughnessFactor;
        dst.baseColorTexture = src.baseColorTexture != kInvalidTexture ? textureIDMap[src.baseColorTexture] : UINT32_MAX;
        dst.emissiveTexture = src.emissiveTexture != kInvalidTexture ? textureIDMap[src.emissiveTexture] : UINT32_MAX;
        dst.metallicRoughnessTexture = src.metallicRoughnessTexture != kInvalidTexture ? textureIDMap[src.metallicRoughnessTexture] : UINT32_MAX;
        dst.normalTexture = src.normalTexture != kInvalidTexture ? textureIDMap[src.normalTexture] : UINT32_MAX;
        dst.transmissionFactor = src.transmissionFactor;
    }

    // 实例
    for (auto& src : newScene.mInstances)
    {
        GPersistentScene.mInstances.emplace_back(src);

        auto& dst = GSInstances.emplace_back();
        dst.transform = src.transform.transform;
        dst.rotation = src.transform.rotation;
        dst.scale = src.transform.scale;
        dst.meshOffset = meshOffsets[src.meshIndex];
        dst.materialIndex = src.materialIndex;
        dst.meshIndex = src.meshIndex;
    }

    // 重新上传完整的实例和材质数组到GPU
    {
        auto [ptr, off] = gpu->AllocateInstance(GSInstances.size());
        std::memcpy(ptr, GSInstances.data(), GSInstances.size() * sizeof(GSInstance));
        GShaderGlobals.firstInstance = off;
        GShaderGlobals.numInstances = GSInstances.size();
    }
    {
        auto [ptr, off] = gpu->AllocateMaterial(GSMaterials.size());
        std::memcpy(ptr, GSMaterials.data(), GSMaterials.size() * sizeof(GSMaterial));
        GShaderGlobals.firstMaterial = off;
        GShaderGlobals.numMaterials = GSMaterials.size();
    }

    // 构建BLAS并重建TLAS
    {
        ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
        GSBLASes.resize(GSMeshes.size());
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
        LOG(Editor, LogDebug, "Rebuilding TLAS");
        ctx->Begin();
        gpu->BuildTLAS(ctx.Get(), GSInstances, GSBLASes, false);
        ctx->End(), ctx.Submit(), ctx.WaitIdle();
    }

    // 上传环境贴图（如果场景自带）
    if (newScene.mEnvMap.has_value() && newScene.mEnvMap->IsValid())
    {
        ImmediateUpload upload(GContext->device.Get(), 128 * (1u << 20));
        upload.Begin();
        gpu->UploadEnvMap(&upload, *newScene.mEnvMap);
        upload.End(), upload.WaitIdle();
        GPersistentScene.mEnvMap = std::move(newScene.mEnvMap);
        GShaderGlobals.useEnvMap = 1u;
        LOG(Editor, LogInfo, "Uploaded scene env map");
    }

    // 接受相机和灯光数据
    {
        if (!newScene.mCameras.empty())
        {
            auto& camera = newScene.mCameras.front();
            vec3 dir = camera.transform.rotation * vec3(0, 0, 1);
            GCamera.center = camera.transform.transform - dir * GCamera.radius;
            GCamera.rot = camera.transform.rotation;
            GCamera.fovY = camera.fovY;
        }
        if (!newScene.mLights.empty())
        {
            auto& light = newScene.mLights.front();
            GShaderGlobals.sunDirection = normalize(light.transform.rotation * float3(0, 0, -1));
            GShaderGlobals.sunIntensity = light.color * light.intensity;
            GShaderGlobals.camMinEV = 0.0f;
            GShaderGlobals.camMaxEV = log2f(light.intensity * 0.25f);
        }
        for (auto& c : newScene.mCameras)
            GPersistentScene.mCameras.emplace_back(c);
        for (auto& l : newScene.mLights)
            GPersistentScene.mLights.emplace_back(l);
    }

    LOG(Editor, LogInfo, "Scene load complete: {} meshes, {} instances, {} materials",
        newScene.mMeshes.size(), newScene.mInstances.size(), newScene.mMaterials.size());

    // 触发渲染器重新配置
    FEState = FERunningEnter;
}

/* ==================== LoadEnvMap ==================== */
static void LoadEnvMap(StringView path)
{
    LOG(Editor, LogInfo, "Loading HDRI env map: {}", path);
    auto* gpu = GContext->gpuScene;
    try
    {
        FTexture2D tex(GLOBAL_ALLOC);
        LoadHDR(tex, path);
        ImmediateUpload upload(GContext->device.Get(), 128 * (1u << 20));
        upload.Begin();
        gpu->UploadEnvMap(&upload, tex);
        upload.End(), upload.WaitIdle();
        GPersistentScene.mEnvMap = std::move(tex);
        GShaderGlobals.useEnvMap = 1u;
        GShaderGlobals.ptAccumualatedFrames = 0;
        // 需要重建渲染器以重新绑定环境贴图资源
        FEState = FERunningEnter;
        LOG(Editor, LogInfo, "HDRI env map loaded successfully");
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to load HDRI env map: {}", path);
    }
}

/* ==================== SaveScene (Task 6) ==================== */
static void SaveScene(StringView path)
{
    LOG(Editor, LogInfo, "Saving scene to: {}", path);
    try
    {
        FileWriter writer(path);
        FSerialize(writer, GPersistentScene);
        GCurrentSavePath = String(path);
        LOG(Editor, LogInfo, "Scene saved successfully");
    }
    catch (...)
    {
        LOG(Editor, LogError, "Failed to save scene to: {}", path);
    }
}

/* ==================== DockSpace + Menu Bar (Tasks 1, 7, 9) ==================== */
static void EditorDockSpaceAndMenuBar()
{
    // 半透明窗口背景
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));

    // DockSpace覆盖整个视口，背景透明以显示backbuffer
    ImGuiID dockspaceID = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

    // 首次启动时设置默认布局 (Task 9)
    static bool firstTime = true;
    if (firstTime)
    {
        firstTime = false;
        ImGui::DockBuilderRemoveNode(dockspaceID);
        ImGui::DockBuilderAddNode(dockspaceID, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceID, ImGui::GetMainViewport()->Size);

        ImGuiID dockLeft, dockCenter;
        ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Left, 0.20f, &dockLeft, &dockCenter);
        ImGuiID dockRight;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.25f, &dockRight, &dockCenter);

        ImGui::DockBuilderDockWindow("Hierarchy", dockLeft);
        ImGui::DockBuilderDockWindow("Inspector", dockLeft);
        ImGui::DockBuilderDockWindow("Camera", dockRight);
        ImGui::DockBuilderDockWindow("Rendering", dockRight);
        ImGui::DockBuilderDockWindow("Profiler", dockRight);
        ImGui::DockBuilderFinish(dockspaceID);
    }

    // 主菜单栏 (Task 7)
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open...", "Ctrl+O"))
            {
                auto path = OpenFileDialog(
                    L"Scene Files\0*.gltf;*.glb;*.fscn\0All Files\0*.*\0",
                    L"Open Scene");
                if (path.has_value())
                    ReplaceScene(path.value());
            }
            if (ImGui::MenuItem("Save", "Ctrl+S"))
            {
                if (!GCurrentSavePath.empty())
                    SaveScene(GCurrentSavePath);
                else
                {
                    auto path = SaveFileDialog(
                        L"Foundation Scene\0*.fscn\0",
                        L"Save Scene As", L"fscn");
                    if (path.has_value())
                        SaveScene(path.value());
                }
            }
            if (ImGui::MenuItem("Save As..."))
            {
                auto path = SaveFileDialog(
                    L"Foundation Scene\0*.fscn\0",
                    L"Save Scene As", L"fscn");
                if (path.has_value())
                    SaveScene(path.value());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load HDRI..."))
            {
                auto path = OpenFileDialog(
                    L"HDR Images\0*.hdr;*.hdri\0All Files\0*.*\0",
                    L"Load HDRI Environment Map");
                if (path.has_value())
                    LoadEnvMap(path.value());
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    ImGui::PopStyleColor(); // WindowBg
}

/* ==================== Hierarchy Panel (Task 8) ==================== */
static void FHierarchyPanel()
{
    if (ImGui::Begin("Hierarchy"))
    {
        if (GSInstances.empty())
        {
            ImGui::TextDisabled("No instances loaded");
        }
        else
        {
            ImGui::Text("%zu instances", GSInstances.size());
            ImGui::Separator();
            for (size_t i = 0; i < GSInstances.size(); i++)
            {
                auto& inst = GSInstances[i];
                char label[128];
                snprintf(label, sizeof(label), "Instance %zu -- Mesh %u, Mat %u",
                         i, inst.meshIndex, inst.materialIndex);
                bool selected = (GSelectedInstance == static_cast<int>(i));
                if (ImGui::Selectable(label, selected))
                    GSelectedInstance = static_cast<int>(i);
            }
        }
    }
    ImGui::End();

    // Inspector面板
    if (ImGui::Begin("Inspector"))
    {
        if (GSelectedInstance >= 0 && GSelectedInstance < static_cast<int>(GSInstances.size()))
        {
            auto& inst = GSInstances[GSelectedInstance];
            ImGui::Text("Instance %d", GSelectedInstance);
            ImGui::Separator();
            bool changed = false;
            changed |= ImGui::DragFloat3("Position", &inst.transform.x, 0.01f);
            changed |= ImGui::DragFloat4("Rotation", &inst.rotation.x, 0.001f);
            changed |= ImGui::DragFloat3("Scale",    &inst.scale.x, 0.01f);
            if (changed)
            {
                // 重新上传实例数组到GPU
                auto* gpu = GContext->gpuScene;
                auto [ptr, off] = gpu->AllocateInstance(GSInstances.size());
                std::memcpy(ptr, GSInstances.data(), GSInstances.size() * sizeof(GSInstance));
                GShaderGlobals.firstInstance = off;
                // 同步到持久场景
                if (GSelectedInstance < static_cast<int>(GPersistentScene.mInstances.size()))
                {
                    auto& pi = GPersistentScene.mInstances[GSelectedInstance];
                    pi.transform.transform = inst.transform;
                    pi.transform.rotation  = inst.rotation;
                    pi.transform.scale     = inst.scale;
                }
                // 重建TLAS并重置PT累积
                {
                    ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
                    ctx->Begin();
                    gpu->BuildTLAS(ctx.Get(), GSInstances, GSBLASes, false);
                    ctx->End(), ctx.Submit(), ctx.WaitIdle();
                }
                GShaderGlobals.ptAccumualatedFrames = 0;
            }
            ImGui::Separator();
            ImGui::Text("Mesh Offset: %u", inst.meshOffset);
            ImGui::Text("Material Index: %u", inst.materialIndex);
            ImGui::Text("Mesh Index: %u", inst.meshIndex);
        }
        else
        {
            ImGui::TextDisabled("No instance selected");
        }
    }
    ImGui::End();
}

void FInit()
{
    // Task 10: FInit状态下显示UI，等待用户加载场景
    auto* renderer = GContext->renderer;
    if (!renderer)
    {
        RendererSetupImGuiOnly(GContext);
        renderer = GContext->renderer;
    }
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    if (GShowImGui)
    {
        EditorDockSpaceAndMenuBar();
        FHierarchyPanel();
    }
    GCamera.Update({});
    GCamera.aspect = GContext->swapchain->GetAspectRatio();
    renderer->ExecuteFrame();
    renderer->EndExecute();

    // 当有场景数据时，转移到 FERunningEnter
    if (!GSInstances.empty())
        FEState = FERunningEnter;
}
RendererConfig GRendererConfig;

static bool rasterOrPT = true;
void FRunningEnter()
{
    // Task 2: 空场景时使用ImGui-only渲染器
    if (GSInstances.empty())
    {
        RendererSetupImGuiOnly(GContext);
        FEState = FEInit;
        return;
    }
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
            ImGui::SeparatorText("Environment Map");
            bool hasEnv = GShaderGlobals.useEnvMap != 0u;
            ImGui::Text(hasEnv ? "HDRI Loaded" : "No HDRI");
            if (hasEnv)
            {
                bool envChanged = false;
                envChanged |= ImGui::SliderFloat("Env Scale", &GShaderGlobals.envMapScale, 0.0f, 10.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
                bool envEnabled = GShaderGlobals.useEnvMap != 0u;
                if (ImGui::Checkbox("Enable Env Map", &envEnabled))
                {
                    GShaderGlobals.useEnvMap = envEnabled ? 1u : 0u;
                    envChanged = true;
                }
                if (envChanged)
                    GShaderGlobals.ptAccumualatedFrames = 0;
            }
            if (ImGui::Button("Load HDRI..."))
            {
                auto path = OpenFileDialog(
                    L"HDR Images\0*.hdr;*.hdri\0All Files\0*.*\0",
                    L"Load HDRI Environment Map");
                if (path.has_value())
                    LoadEnvMap(path.value());
            }
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
void FRunning()
{
    auto* renderer = GContext->renderer;
    // New frame
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();
    if (GShowImGui)
    {
        EditorDockSpaceAndMenuBar();
        FHierarchyPanel();
        FRunningImGui();
    }
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
            GShowImGui = !GShowImGui;
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
