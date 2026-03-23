#include "Editor.hpp"
#include "Texture.hpp"
#include <RenderCore/ImmediateContext.hpp>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <ImGuiFileDialog.h>
#include <Math/Decompose.hpp>
#include <filesystem>

FEditorState FEState = FEInitEnter;
/* -- Scene Data -- */
static Vector<GSInstance> GSInstances(GLOBAL_ALLOC);
static Vector<GSMaterial> GSMaterials(GLOBAL_ALLOC);
static Vector<GSMesh> GSMeshes(GLOBAL_ALLOC);
static Vector<uint32_t> GSBLASes(GLOBAL_ALLOC);
/* -- CPU Scene for saving -- */
static FScene GScene(GLOBAL_ALLOC);
static String GCurrentSavePath;
static int GSelectedInstance = -1;
static int GSelectedMaterial = -1;
static bool GShowImGui = true;
static PTReadbackHandles GPTReadback;
/* -- Offline Rendering State -- */
enum class ERenderFormat { HDR, SDR };
static ERenderFormat GRenderFormat = ERenderFormat::HDR;
static int GRenderTargetSamples = 0;
static int GRenderSamplePopupInput = 4096;
static bool GOpenRenderPopup = false;
static String GRenderOutputPath;
/* -- Gizmo -- */
static ImGuizmo::OPERATION GGizmoOp = ImGuizmo::TRANSLATE;
static ImGuizmo::MODE GGizmoMode = ImGuizmo::WORLD;
/* -- Camera & Globals -- */
static UBO GShaderGlobals;
static FArcballCamera GCamera{
    .center = float3{0, 0, 0},
    .radius = 1.0f,
    .zNear = 0.1f,
    .fovY = radians(60.f),
};

enum class ERendererMode
{
    PathTracer,
    Raster
};
static ERendererMode GRendererMode = ERendererMode::PathTracer;

/* -- 前向声明 -- */
static void ReplaceScene(StringView path);
static void SaveScene(StringView path);
static void LoadEnvMap(StringView path);
static void EditorDockSpaceAndMenuBar();
static void FHierarchyPanel();

/* ==================== ReplaceScene ==================== */
static void ReplaceScene(StringView path)
{
    LOG(Editor, LogInfo, "Loading scene: {}", path);
    GScene = FScene(GLOBAL_ALLOC);
    try
    {
        LoadScene(path, GScene);
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
    
    GSelectedInstance = -1;
    GSelectedMaterial = -1;

    auto* gpu = GContext->gpuScene;
    gpu->Reset();

    Vector<uint32_t> meshOffsets(GLOBAL_ALLOC);
    Vector<uint32_t> textureIDMap(GScene.mTextures.size(), GLOBAL_ALLOC);

    LOG(Editor, LogInfo, "Uploading new scene data to GPU");
    // 上传Mesh和Texture
    {
        ImmediateUpload upload(GContext->device.Get(), 128 * (1u << 20));
        upload.Begin();
        for (auto& src : GScene.mMeshes)
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
        for (int id = 0; auto& src : GScene.mTextures)
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

    // 材质：映射纹理索引
    for (auto& src : GScene.mMaterials)
    {
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
        dst.ior = src.ior;
    }

    // 实例
    for (auto& src : GScene.mInstances)
    {
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
        auto res = gpu->UpdateGPUScene(GSInstances, GSMaterials);
        GShaderGlobals.firstInstance = res.firstInstance;
        GShaderGlobals.numInstances  = res.numInstances;
        GShaderGlobals.firstMaterial = res.firstMaterial;
        GShaderGlobals.numMaterials  = res.numMaterials;
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
    if (GScene.mEnvMap.has_value() && GScene.mEnvMap->IsValid())
    {
        ImmediateUpload upload(GContext->device.Get(), 128 * (1u << 20));
        upload.Begin();
        gpu->UploadEnvMap(&upload, *GScene.mEnvMap);
        upload.End(), upload.WaitIdle();
        GScene.mEnvMap = std::move(GScene.mEnvMap);
        GShaderGlobals.useEnvMap = 1u;
        LOG(Editor, LogInfo, "Uploaded scene env map");
    }

    // 接受相机和灯光数据
    {
        if (!GScene.mCameras.empty())
        {
            auto& camera = GScene.mCameras.front();
            vec3 dir = camera.transform.rotation * vec3(0, 0, 1);
            GCamera.center = camera.transform.transform - dir * GCamera.radius;
            GCamera.rot = camera.transform.rotation;
            GCamera.fovY = camera.fovY;
        }
        if (!GScene.mLights.empty())
        {
            auto& light = GScene.mLights.front();
            GShaderGlobals.sunDirection = normalize(light.transform.rotation * float3(0, 0, -1));
            GShaderGlobals.sunIntensity = light.color * light.intensity;
        GShaderGlobals.camEV = log2f(light.intensity * 0.25f);
        }
        for (auto& c : GScene.mCameras)
            GScene.mCameras.emplace_back(c);
        for (auto& l : GScene.mLights)
            GScene.mLights.emplace_back(l);
    }

    LOG(Editor, LogInfo, "Scene load complete: {} meshes, {} instances, {} materials",
        GScene.mMeshes.size(), GScene.mInstances.size(), GScene.mMaterials.size());

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
        GScene.mEnvMap = std::move(tex);
        GShaderGlobals.useEnvMap = 1u;
        GShaderGlobals.ptAccumulatedFrames = 0;
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
        FSerialize(writer, GScene);
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

        ImGuiID dockLeftTop, dockLeftBottom;
        ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Up, 0.5f, &dockLeftTop, &dockLeftBottom);
        ImGui::DockBuilderDockWindow("Hierarchy", dockLeftTop);
        ImGui::DockBuilderDockWindow("Inspector", dockLeftBottom);
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
            if (ImGui::MenuItem("Open Scene..."))
            {
                IGFD::FileDialogConfig config;
                config.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog(
                    "OpenSceneDlg", "Open Scene", ".gltf,.glb,.fscn", config);
            }
            if (ImGui::MenuItem("Open HDR..."))
            {
                IGFD::FileDialogConfig config;
                config.path = ".";
                ImGuiFileDialog::Instance()->OpenDialog(
                    "OpenHDRDlg", "Open HDR Environment Map", ".hdr,.hdri", config);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S"))
            {
                if (!GCurrentSavePath.empty())
                    SaveScene(GCurrentSavePath);
            }
            if (ImGui::MenuItem("Save As..."))
            {
                IGFD::FileDialogConfig config;
                config.path = ".";
                config.flags = ImGuiFileDialogFlags_ConfirmOverwrite;
                ImGuiFileDialog::Instance()->OpenDialog(
                    "SaveAsDlg", "Save Scene As", ".fscn", config);
            }
            ImGui::Separator();
            if (GRendererMode == ERendererMode::PathTracer && !GSInstances.empty())
            {
                if (ImGui::MenuItem("Render .hdr..."))
                {
                    IGFD::FileDialogConfig config;
                    config.path = ".";
                    config.flags = ImGuiFileDialogFlags_ConfirmOverwrite;
                    ImGuiFileDialog::Instance()->OpenDialog(
                        "RenderHDRDlg", "Save Render Output", ".hdr", config);
                }
                if (ImGui::MenuItem("Render .png..."))
                {
                    IGFD::FileDialogConfig config;
                    config.path = ".";
                    config.flags = ImGuiFileDialogFlags_ConfirmOverwrite;
                    ImGuiFileDialog::Instance()->OpenDialog(
                        "RenderSDRDlg", "Save Render Output", ".png", config);
                }
            }
            ImGui::EndMenu();
        }

        // Render Settings modal popup (opened after file dialog)
        if (GOpenRenderPopup)
        {
            ImGui::OpenPopup("Render Settings");
            GOpenRenderPopup = false;
        }
        if (ImGui::BeginPopupModal("Render Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const char* fmtLabel = GRenderFormat == ERenderFormat::HDR ? "HDR" : "SDR (PNG)";
            ImGui::Text("Configure %s render:", fmtLabel);
            ImGui::Text("Output: %s", GRenderOutputPath.c_str());
            ImGui::Separator();
            ImGui::InputInt("Samples (frames)", &GRenderSamplePopupInput);
            if (GRenderSamplePopupInput < 1) GRenderSamplePopupInput = 1;
            if (ImGui::Button("Start Render"))
            {
                GRenderTargetSamples = GRenderSamplePopupInput;
                GShaderGlobals.ptAccumulatedFrames = 0;
                FEState = FERendering;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Right-aligned PT / Raster toggle
        if (!GSInstances.empty()) {
            const char* labelPT = " PT ";
            const char* labelRaster = "RSTR";
            float btnW_PT = ImGui::CalcTextSize(labelPT).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float btnW_R  = ImGui::CalcTextSize(labelRaster).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float totalW  = btnW_PT + btnW_R;
            float avail   = ImGui::GetContentRegionAvail().x;
            if (avail > totalW)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - totalW);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            if (GRendererMode == ERendererMode::PathTracer)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            else
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            if (ImGui::Button(labelPT))
            {
                if (GRendererMode != ERendererMode::PathTracer) { GRendererMode = ERendererMode::PathTracer; FEState = FERunningEnter; }
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();

            if (GRendererMode == ERendererMode::Raster)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            else
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            if (ImGui::Button(labelRaster))
            {
                if (GRendererMode != ERendererMode::Raster) { GRendererMode = ERendererMode::Raster; FEState = FERunningEnter; }
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        ImGui::EndMainMenuBar();
    }

    // ImGuiFileDialog 弹窗渲染
    ImVec2 minSize(600, 400);
    ImVec2 maxSize(FLT_MAX, FLT_MAX);

    if (ImGuiFileDialog::Instance()->Display("OpenSceneDlg", ImGuiWindowFlags_NoCollapse, minSize, maxSize))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string filePath = ImGuiFileDialog::Instance()->GetFilePathName();
            ReplaceScene(filePath.c_str());
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display("OpenHDRDlg", ImGuiWindowFlags_NoCollapse, minSize, maxSize))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string filePath = ImGuiFileDialog::Instance()->GetFilePathName();
            LoadEnvMap(filePath.c_str());
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display("SaveAsDlg", ImGuiWindowFlags_NoCollapse, minSize, maxSize))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            std::string filePath = ImGuiFileDialog::Instance()->GetFilePathName();
            SaveScene(filePath.c_str());
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display("RenderHDRDlg", ImGuiWindowFlags_NoCollapse, minSize, maxSize))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            GRenderOutputPath = ImGuiFileDialog::Instance()->GetFilePathName();
            GRenderFormat = ERenderFormat::HDR;
            GOpenRenderPopup = true;
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ImGuiFileDialog::Instance()->Display("RenderSDRDlg", ImGuiWindowFlags_NoCollapse, minSize, maxSize))
    {
        if (ImGuiFileDialog::Instance()->IsOk())
        {
            GRenderOutputPath = ImGuiFileDialog::Instance()->GetFilePathName();
            GRenderFormat = ERenderFormat::SDR;
            GOpenRenderPopup = true;
        }
        ImGuiFileDialog::Instance()->Close();
    }

    ImGui::PopStyleColor(); // WindowBg
}

/* ==================== Hierarchy Panel (Task 8) ==================== */
static void FHierarchyPanel()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
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
    ImGui::PopStyleColor();

    // Inspector面板 (Instance)
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Inspector"))
    {
        if (GSelectedInstance >= 0 && GSelectedInstance < static_cast<int>(GScene.mInstances.size()))
        {
            auto& pi = GScene.mInstances[GSelectedInstance];
            ImGui::Text("Instance %d", GSelectedInstance);
            ImGui::Separator();
            bool changed = false;
            changed |= ImGui::DragFloat3("Position", &pi.transform.transform.x, 0.01f);
            changed |= ImGui::DragFloat4("Rotation", &pi.transform.rotation.x, 0.001f);
            changed |= ImGui::DragFloat3("Scale",    &pi.transform.scale.x, 0.01f);

            // -- Gizmo控件 --
            ImGui::Separator();
            if (ImGui::RadioButton("Translate (G)", GGizmoOp == ImGuizmo::TRANSLATE))
                GGizmoOp = ImGuizmo::TRANSLATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Rotate (R)", GGizmoOp == ImGuizmo::ROTATE))
                GGizmoOp = ImGuizmo::ROTATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Scale (Q)", GGizmoOp == ImGuizmo::SCALE))
                GGizmoOp = ImGuizmo::SCALE;
            if (GGizmoOp != ImGuizmo::SCALE)
            {
                if (ImGui::RadioButton("Local", GGizmoMode == ImGuizmo::LOCAL))
                    GGizmoMode = ImGuizmo::LOCAL;
                ImGui::SameLine();
                if (ImGui::RadioButton("World", GGizmoMode == ImGuizmo::WORLD))
                    GGizmoMode = ImGuizmo::WORLD;
            }

            // 构建模型矩阵 (TRS -> mat4)
            mat4 modelMatrix = translate(mat4(1.0f), vec3(pi.transform.transform))
                             * mat4_cast(pi.transform.rotation)
                             * glm::scale(mat4(1.0f), vec3(pi.transform.scale));

            // ImGuizmo渲染
            ImGuizmo::BeginFrame();
            ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
            auto& io = ImGui::GetIO();
            ImGuizmo::SetRect(0,0,io.DisplaySize.x, io.DisplaySize.y);
            // 注意: ImGuizmo使用列主序 float[16]，与GLM mat4内存布局一致            
            if (ImGuizmo::Manipulate(&GCamera.view[0][0], &GCamera.proj[0][0],
                                     GGizmoOp, GGizmoMode, &modelMatrix[0][0]))
            {
                // 分解回 TRS
                float3 newTranslation;
                quat newRotation;
                float3 newScale;
                Foundation::Math::decompose(modelMatrix, newScale, newRotation, newTranslation);
                pi.transform.transform = newTranslation;
                pi.transform.rotation  = newRotation;
                pi.transform.scale     = newScale;
                changed = true;
            }
            if (changed)
            {
                // 同步CPU场景到GPU侧数据
                auto& inst = GSInstances[GSelectedInstance];
                inst.transform = pi.transform.transform;
                inst.rotation  = pi.transform.rotation;
                inst.scale     = pi.transform.scale;
                // 重新上传实例数组到GPU
                auto* gpu = GContext->gpuScene;
                auto res = gpu->UpdateGPUScene(GSInstances, GSMaterials);
                GShaderGlobals.firstInstance = res.firstInstance;
                GShaderGlobals.firstMaterial = res.firstMaterial;
                GShaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::Separator();
            auto& inst = GSInstances[GSelectedInstance];
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
    ImGui::PopStyleColor();
}


void FInit()
{
    // 当有场景数据时，转移到 FERunningEnter
    if (!GSInstances.empty())
        FEState = FERunningEnter;
    else
    {
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
        float dt = ImGui::GetIO().DeltaTime;
        bool cameraUpdated = false;
        cameraUpdated |= GCamera.UpdateMovement(dt);
        GCamera.Update({});
        GCamera.aspect = GContext->swapchain->GetAspectRatio();
        renderer->ExecuteFrame();
        renderer->EndExecute();
    }
}
RendererConfig GRendererConfig;

void FRunningEnter()
{
    // 重建渲染器时先失效旧的 PT readback 句柄
    GPTReadback = {};
    RendererScene scene{
        .gsGlobals = &GShaderGlobals,
        .gsInstances = &GSInstances,
        .gsMaterials = &GSMaterials,
        .gsMeshes = &GSMeshes,
        .gsBLASes = &GSBLASes
    };
    if (GRendererMode == ERendererMode::PathTracer)
        PathTracerSetup(GContext, GRendererConfig, scene, GPTReadback);
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Camera"))
    {
        ImGui::TextUnformatted(FArcballCamera::kControlsText);
        ImGui::Separator();
        cameraUpdated |= ImGui::SliderFloat3("Cam Center", &GCamera.center.x, -50.0f, 50.0f);
        cameraUpdated |= ImGui::SliderFloat("Cam Radius", &GCamera.radius, 0.0f, 100.0f);
        cameraUpdated |= ImGui::SliderAngle("Cam FOV Y", &GCamera.fovY);
        cameraUpdated |= ImGui::SliderFloat("Aperture", &GShaderGlobals.aperture, 1e-5f, 1.0f, "%.5f", ImGuiSliderFlags_Logarithmic);
        cameraUpdated |= ImGui::SliderFloat("Focal Distance", &GShaderGlobals.focalDistance, 0.1f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Exposure (EV)", &GShaderGlobals.camEV, -16.0f, 16.0f);
        ImGui::Separator();
        ImGui::SliderFloat("WASD Speed", &GCamera.moveSpeed, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Rendering"))
    {
        static float lodLogThreshold = 3;
        ImGui::SliderFloat("LOD ", &lodLogThreshold, 0, 8);
        if (GRendererMode == ERendererMode::PathTracer)
        {
            ImGui::Text("PT Accumulation: %d", GShaderGlobals.ptAccumulatedFrames);
            ImGui::SliderInt("PT Bounces", &GShaderGlobals.ptMaxBounces, 1, 64);
            ImGui::SliderFloat("Firefly Clamp", &GShaderGlobals.ptFireflyClamp, 1.0f, 100.0f, "%.1f");
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
            const char* items[] = {"Diffuse", "Specular"};
            const unsigned values[] = {kViewAOVDiffuse, kViewAOVSpecular};
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
                    GShaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::TextDisabled("Drag & drop .hdr/.hdri to load");
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
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
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
                ImGui::Text("CPU to Present: %.3fms\nP2P: %.3fms (%.1f FPS)\nGPU: %.3fms\n"
                            "CPU/GPU dt: %.3fms",
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
    ImGui::PopStyleColor();
}
/* ==================== HDR Rendering State ==================== */
static void DoHDRReadback()
{
    auto* renderer = GContext->renderer;
    auto [w, h] = renderer->GetSwapchainExtent();
    const size_t pixelCount = static_cast<size_t>(w) * h;
    const size_t imageBytes = pixelCount * 4 * sizeof(float); // RGBA32F

    auto* diffuseTex  = renderer->DerefResource(GPTReadback.diffuse).Get<RHITexture*>();
    auto* specularTex = renderer->DerefResource(GPTReadback.specular).Get<RHITexture*>();

    auto readbackBuf = GContext->device->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Readback,
                     .hostAccess = RHIResourceHostAccess::ReadWrite,
                     .coherent = true},
        .usage = RHIBufferUsageBits::TransferDestination,
        .size = imageBytes * 2});

    {
        ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
        auto* cmd = ctx.Get();
        cmd->Begin();
        cmd->BeginTransition();
        cmd->SetImageTransition(diffuseTex, {
            .srcAccess = RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
            .dstAccess = RHIResourceAccessBits::TransferRead,
            .srcStage = RHIPipelineStageBits::BottomOfPipe,
            .dstStage = RHIPipelineStageBits::Transfer,
            .srcImgLayout = RHITextureLayout::General,
            .dstImgLayout = RHITextureLayout::TransferSrc,
            .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->SetImageTransition(specularTex, {
            .srcAccess = RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
            .dstAccess = RHIResourceAccessBits::TransferRead,
            .srcStage = RHIPipelineStageBits::BottomOfPipe,
            .dstStage = RHIPipelineStageBits::Transfer,
            .srcImgLayout = RHITextureLayout::General,
            .dstImgLayout = RHITextureLayout::TransferSrc,
            .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->EndTransition();
        cmd->CopyImageToBuffer(diffuseTex, RHITextureLayout::TransferSrc, readbackBuf.Get(),
            {{{.dstBufferOffset = 0,
               .srcLayer = {.aspect = RHITextureAspectFlagBits::Color},
               .extent = {w, h, 1}}}});
        cmd->CopyImageToBuffer(specularTex, RHITextureLayout::TransferSrc, readbackBuf.Get(),
            {{{.dstBufferOffset = static_cast<uint32_t>(imageBytes),
               .srcLayer = {.aspect = RHITextureAspectFlagBits::Color},
               .extent = {w, h, 1}}}});
        cmd->BeginTransition();
        cmd->SetImageTransition(diffuseTex, {
            .srcAccess = RHIResourceAccessBits::TransferRead,
            .dstAccess = RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
            .srcStage = RHIPipelineStageBits::Transfer,
            .dstStage = RHIPipelineStageBits::TopOfPipe,
            .srcImgLayout = RHITextureLayout::TransferSrc,
            .dstImgLayout = RHITextureLayout::General,
            .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->SetImageTransition(specularTex, {
            .srcAccess = RHIResourceAccessBits::TransferRead,
            .dstAccess = RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
            .srcStage = RHIPipelineStageBits::Transfer,
            .dstStage = RHIPipelineStageBits::TopOfPipe,
            .srcImgLayout = RHITextureLayout::TransferSrc,
            .dstImgLayout = RHITextureLayout::General,
            .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->EndTransition();
        cmd->End();
        ctx.Submit();
        ctx.WaitIdle();
    }

    auto* mapped = readbackBuf->Map<float>();
    const float* diffuseData  = mapped;
    const float* specularData = mapped + pixelCount * 4;
    Vector<float> combined(pixelCount * 4, GLOBAL_ALLOC);
    for (size_t i = 0; i < pixelCount; ++i)
    {
        combined[i * 4 + 0] = diffuseData[i * 4 + 0] + specularData[i * 4 + 0];
        combined[i * 4 + 1] = diffuseData[i * 4 + 1] + specularData[i * 4 + 1];
        combined[i * 4 + 2] = diffuseData[i * 4 + 2] + specularData[i * 4 + 2];
        combined[i * 4 + 3] = 1.0f;
    }
    readbackBuf->Unmap();

    const char* hdrPath = GRenderOutputPath.empty() ? "render_output.hdr" : GRenderOutputPath.c_str();
    SaveHDR(combined.data(), static_cast<int>(w), static_cast<int>(h), hdrPath);
    LOG(Editor, LogInfo, "HDR image saved to {} ({}x{}, {} samples)",
        hdrPath, w, h, GShaderGlobals.ptAccumulatedFrames);
}

/* ==================== SDR (PNG) Rendering State ==================== */
static void DoSDRReadback()
{
    auto* renderer = GContext->renderer;
    auto [w, h] = renderer->GetSwapchainExtent();
    const size_t pixelCount = static_cast<size_t>(w) * h;
    const size_t imageBytes = pixelCount * 4; // RGBA8 — 1 byte per channel

    auto* sdrTex = renderer->DerefResource(GPTReadback.sdrRenderTarget).Get<RHITexture*>();

    auto readbackBuf = GContext->device->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Readback,
                     .hostAccess = RHIResourceHostAccess::ReadWrite,
                     .coherent = true},
        .usage = RHIBufferUsageBits::TransferDestination,
        .size = imageBytes});

    {
        ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
        auto* cmd = ctx.Get();
        cmd->Begin();
        cmd->BeginTransition();
        cmd->SetImageTransition(sdrTex, {
            .srcAccess = RHIResourceAccessBits::RenderTargetRead | RHIResourceAccessBits::RenderTargetWrite,
            .dstAccess = RHIResourceAccessBits::TransferRead,
            .srcStage = RHIPipelineStageBits::RenderTargetOutput,
            .dstStage = RHIPipelineStageBits::Transfer,
            .srcImgLayout = RHITextureLayout::RenderTarget,
            .dstImgLayout = RHITextureLayout::TransferSrc,
            .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->EndTransition();
        cmd->CopyImageToBuffer(sdrTex, RHITextureLayout::TransferSrc, readbackBuf.Get(),
            {{{.dstBufferOffset = 0,
               .srcLayer = {.aspect = RHITextureAspectFlagBits::Color},
               .extent = {w, h, 1}}}});
        cmd->BeginTransition();
        cmd->SetImageTransition(sdrTex, {
            .srcAccess = RHIResourceAccessBits::TransferRead,
            .dstAccess = RHIResourceAccessBits::RenderTargetRead | RHIResourceAccessBits::RenderTargetWrite,
            .srcStage = RHIPipelineStageBits::Transfer,
            .dstStage = RHIPipelineStageBits::TopOfPipe,
            .srcImgLayout = RHITextureLayout::TransferSrc,
            .dstImgLayout = RHITextureLayout::RenderTarget,
            .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->EndTransition();
        cmd->End();
        ctx.Submit();
        ctx.WaitIdle();
    }

    auto* mapped = readbackBuf->Map<unsigned char>();
    const char* pngPath = GRenderOutputPath.empty() ? "render_output.png" : GRenderOutputPath.c_str();
    SavePNG(mapped, static_cast<int>(w), static_cast<int>(h), pngPath);
    readbackBuf->Unmap();

    LOG(Editor, LogInfo, "SDR image saved to {} ({}x{}, {} samples)",
        pngPath, w, h, GShaderGlobals.ptAccumulatedFrames);
}

void FRendering()
{
    auto* renderer = GContext->renderer;
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();

    bool cancelRendering = false;
    // 顶部全宽进度条
    {
        auto& io = ImGui::GetIO();
        float margin = 16.0f;
        float barH = 28.0f;
        float barW = io.DisplaySize.x - margin * 2.0f;
        ImGui::SetNextWindowPos(ImVec2(margin, margin));
        ImGui::SetNextWindowSize(ImVec2(barW, barH + ImGui::GetStyle().WindowPadding.y * 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
        ImGui::Begin("##RenderProgressBar", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_AlwaysAutoResize);
        float fraction = GRenderTargetSamples > 0
            ? static_cast<float>(GShaderGlobals.ptAccumulatedFrames) / static_cast<float>(GRenderTargetSamples)
            : 0.0f;
        char overlay[128];
        snprintf(overlay, sizeof(overlay), "%d / %d samples",
                 GShaderGlobals.ptAccumulatedFrames, GRenderTargetSamples);
        ImGui::ProgressBar(fraction, ImVec2(barW, barH), overlay);
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    // 底部右角取消按钮
    {
        auto& io = ImGui::GetIO();
        const char* cancelLabel = "  Cancel  ";
        ImVec2 textSize = ImGui::CalcTextSize(cancelLabel);
        float btnW = textSize.x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float btnH = textSize.y + ImGui::GetStyle().FramePadding.y * 2.0f;
        float margin = 24.0f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - btnW - margin, io.DisplaySize.y - btnH - margin));
        ImGui::SetNextWindowSize(ImVec2(btnW, btnH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
        ImGui::Begin("##RenderCancel", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
            ImGuiWindowFlags_AlwaysAutoResize);
        cancelRendering = ImGui::Button(cancelLabel, ImVec2(btnW, btnH));
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    GShaderGlobals.frameNumber = renderer->GetFrame();
    renderer->ExecuteFrame();
    renderer->EndExecute();
    GShaderGlobals.ptAccumulatedFrames++;

    if (cancelRendering)
    {
        FEState = FERunning;
    }
    else if (GShaderGlobals.ptAccumulatedFrames >= static_cast<uint32_t>(GRenderTargetSamples))
    {
        if (GRenderFormat == ERenderFormat::HDR)
        {
            if (GPTReadback.diffuse != kInvalidHandle && GPTReadback.specular != kInvalidHandle)
                DoHDRReadback();
        }
        else
        {
            if (GPTReadback.sdrRenderTarget != kInvalidHandle)
                DoSDRReadback();
        }
        FEState = FERunning;
    }
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
    float dt = ImGui::GetIO().DeltaTime;
    cameraUpdated |= GCamera.UpdateMovement(dt);
    GCamera.Update({});
    GCamera.aspect = GContext->swapchain->GetAspectRatio();
    GShaderGlobals.frameNumber = renderer->GetFrame();
    GShaderGlobals.view = GCamera.view;
    GShaderGlobals.proj = GCamera.proj;
    GShaderGlobals.inverseView = inverse(GShaderGlobals.view);
    GShaderGlobals.inverseViewProj = inverse(GShaderGlobals.proj * GShaderGlobals.view);
    GShaderGlobals.zNear = GCamera.zNear;
    GShaderGlobals.projPlanes = planeSymmetric(GShaderGlobals.proj);

    GShaderGlobals.camPosition = GCamera.position;
    GShaderGlobals.camDirection = GCamera.rot * float3(0, 0, -1);
    GShaderGlobals.fbWidth = static_cast<float>(renderer->GetSwapchainExtent().x);
    GShaderGlobals.fbHeight = static_cast<float>(renderer->GetSwapchainExtent().y);
    if (cameraUpdated)
        GShaderGlobals.ptAccumulatedFrames = 0, cameraUpdated = false;
    renderer->ExecuteFrame();
    renderer->EndExecute();
    GShaderGlobals.ptAccumulatedFrames++;
}

/* -- 拖放文件处理：根据扩展名分发到对应加载函数 -- */
static void HandleFile(const char* filePath)
{
    auto ext = std::filesystem::path(filePath).extension().string();
    // 统一转小写
    for (auto& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".gltf" || ext == ".glb" || ext == ".fscn")
    {
        ReplaceScene(filePath);
    }
    else if (ext == ".hdr" || ext == ".hdri")
    {
        LoadEnvMap(filePath);
    }
    else
    {
        LOG(Editor, LogWarn, "Unknown file type dropped: '{}'", filePath);
    }
}

/* -- */
void FInitEnter()
{
    if (GContext->args.size() < 2)
    {
        LOG(Editor, LogInfo, "No scene path provided, starting with empty scene");
        RendererSetupImGuiOnly(GContext);
    }
    else
        for (int i = 1; i < GContext->args.size(); i++)
            HandleFile(GContext->args[i]);
    FEState = FEInit;
}

bool EditorProcessEvent(SDL_Event* event)
{
    // 处理拖放文件事件
    if (event->type == SDL_EVENT_DROP_FILE)
    {
        const char* droppedFile = event->drop.data;
        if (droppedFile)
        {
            LOG(Editor, LogInfo, "File dropped: {}", droppedFile);
            HandleFile(droppedFile);
        }
    }
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
        // Gizmo快捷键
        if (event->key.key == SDLK_G)
            GGizmoOp = ImGuizmo::TRANSLATE;
        if (event->key.key == SDLK_R)
            GGizmoOp = ImGuizmo::ROTATE;
        if (event->key.key == SDLK_Q)
            GGizmoOp = ImGuizmo::SCALE;
    }
    ImGui_ImplFoundation_ProcessEvent(event);
    auto& io = ImGui::GetIO();
    if (!io.WantCaptureMouse && !ImGuizmo::IsUsing())
        cameraUpdated |= GCamera.Update(*event);
    // 始终让相机追踪WASD键状态（仅当ImGui不需要键盘时）
    if (!io.WantCaptureKeyboard)
    {
        if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP)
        {
            bool pressed = (event->type == SDL_EVENT_KEY_DOWN);
            switch (event->key.key)
            {
            case SDLK_W: GCamera.keyW = pressed; break;
            case SDLK_A: GCamera.keyA = pressed; break;
            case SDLK_S: GCamera.keyS = pressed; break;
            case SDLK_D: GCamera.keyD = pressed; break;
            case SDLK_LSHIFT: case SDLK_RSHIFT: GCamera.keyShift = pressed; break;
            default: break;
            }
        }
    }
    else
    {
        // ImGui要求键盘时，清除所有移动键状态防止卡键
        GCamera.keyW = GCamera.keyA = GCamera.keyS = GCamera.keyD = GCamera.keyShift = false;
    }
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
    case FERendering:
        FRendering();
        break;
    default:
        return true;
    }
    return false;
}