#include <cmath>
#include <cfloat>
#include <algorithm>
#include <nfd.h>
#include <Math/Decompose.hpp>
#include <imgui_internal.h>
#include "EditorState.hpp"
#include "Scene/Mesh.hpp"

static void DrawLightGizmos();

struct PTSPPOption
{
    const char* label;
    uint32_t samplesPerPixel;
    uint32_t dispatchTileSide;
};

static constexpr PTSPPOption kPTSPPOptions[] = {
    {"1/64", 1u, 8u},
    {"1/49", 1u, 7u},
    {"1/36", 1u, 6u},
    {"1/25", 1u, 5u},
    {"1/16", 1u, 4u},
    {"1/9",  1u, 3u},
    {"1/4",  1u, 2u},
    {"1",    1u, 1u},
    {"2",    2u, 1u},
    {"3",    3u, 1u},
    {"4",    4u, 1u},
    {"5",    5u, 1u},
};
static constexpr int kPTSPPOptionCount = static_cast<int>(sizeof(kPTSPPOptions) / sizeof(kPTSPPOptions[0]));

static int PTSPPOptionIndex(UBO const& ubo)
{
    uint32_t samplesPerPixel = PTSamplesPerDispatch(ubo);
    uint32_t dispatchTileSide = PTDispatchTileSide(ubo);
    for (int i = 0; i < kPTSPPOptionCount; ++i)
        if (kPTSPPOptions[i].samplesPerPixel == samplesPerPixel &&
            kPTSPPOptions[i].dispatchTileSide == dispatchTileSide)
            return i;
    return 3; // 1/25 SPP
}

static void SetPTSPPOption(int index)
{
    PTSPPOption const& option = kPTSPPOptions[std::clamp(index, 0, kPTSPPOptionCount - 1)];
    if (GEditor.shaderGlobals.ptSamplesPerPixel != option.samplesPerPixel ||
        GEditor.shaderGlobals.ptDispatchTileSide != option.dispatchTileSide)
    {
        GEditor.shaderGlobals.ptSamplesPerPixel = option.samplesPerPixel;
        GEditor.shaderGlobals.ptDispatchTileSide = option.dispatchTileSide;
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
    }
}

static void DrawAperturePreview(uint32_t blades, float rotation, float ratio)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float size = avail.x < 160.0f ? avail.x : 160.0f;
    size = size < 80.0f ? 80.0f : size;

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("AperturePreview", ImVec2(size, size));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 center = ImVec2(p0.x + size * 0.5f, p0.y + size * 0.5f);
    ImU32 bg = ImGui::GetColorU32(ImVec4(0.12f, 0.12f, 0.12f, 0.55f));
    ImU32 fill = ImGui::GetColorU32(ImVec4(0.72f, 0.72f, 0.72f, 0.55f));
    ImU32 outline = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.90f));

    drawList->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size), bg, 4.0f);
    drawList->AddRect(p0, ImVec2(p0.x + size, p0.y + size), outline, 4.0f);

    ImVec2 points[64];
    int pointCount = blades >= 3u ? static_cast<int>(blades) : 64;
    pointCount = pointCount > 64 ? 64 : pointCount;

    float ratioSafe = ratio > 1e-3f ? ratio : 1e-3f;
    float xScale = 1.0f / ratioSafe;
    float fitScale = size * 0.42f / (xScale > 1.0f ? xScale : 1.0f);

    for (int i = 0; i < pointCount; ++i)
    {
        float theta = rotation + (2.0f * pi<float>() * float(i)) / float(pointCount);
        points[i] = ImVec2(center.x + std::cos(theta) * xScale * fitScale,
                           center.y + std::sin(theta) * fitScale);
    }

    drawList->AddConvexPolyFilled(points, pointCount, fill);
    drawList->AddPolyline(points, pointCount, outline, ImDrawFlags_Closed, 2.0f);
}

static bool IsSelectedInstanceValid()
{
    return GEditor.doc.selectedInstance >= 0 &&
           GEditor.doc.selectedInstance < static_cast<int>(GEditor.doc.instances.size()) &&
           GEditor.doc.selectedInstance < static_cast<int>(GEditor.doc.scene.mInstances.size());
}

static bool IsSelectedCurveInstanceValid()
{
    return GEditor.doc.selectedCurveInstance >= 0 &&
           GEditor.doc.selectedCurveInstance < static_cast<int>(GEditor.doc.curveInstances.size()) &&
           GEditor.doc.selectedCurveInstance < static_cast<int>(GEditor.doc.scene.mCurveInstances.size());
}

static int GetSelectedMaterialIndex()
{
    if (IsSelectedInstanceValid())
        return static_cast<int>(GEditor.doc.instances[GEditor.doc.selectedInstance].materialIndex);
    if (IsSelectedCurveInstanceValid())
        return static_cast<int>(GEditor.doc.curveInstances[GEditor.doc.selectedCurveInstance].materialIndex);
    return GEditor.doc.selectedMaterial;
}

static bool IsMaterialIndexValid(int materialIndex)
{
    return materialIndex >= 0 &&
           materialIndex < static_cast<int>(GEditor.doc.materials.size()) &&
           materialIndex < static_cast<int>(GEditor.doc.scene.mMaterials.size());
}

static void SyncMaterialToGPU(uint32_t materialIndex)
{
    auto& src = GEditor.doc.scene.mMaterials[materialIndex];
    auto& dst = GEditor.doc.materials[materialIndex];
    dst.baseColorFactor = src.baseColorFactor;
    dst.emissiveFactor = src.emissiveFactor * src.emissiveFactor.w;
    dst.metallicFactor = src.metallicFactor;
    dst.roughnessFactor = src.roughnessFactor;
    dst.transmissionFactor = src.transmissionFactor;
    dst.ior = src.ior;
    dst.specularFactor = src.specularFactor;
    dst.specularColorFactor = src.specularColorFactor;
    dst.anisotropyStrength = src.anisotropyStrength;
    dst.anisotropyRotation = src.anisotropyRotation;
    dst.subsurfaceFactor = src.subsurfaceFactor;
    dst.subsurfaceScale = src.subsurfaceScale;
    dst.subsurfaceColor = src.subsurfaceColor;
    dst.subsurfaceRadius = src.subsurfaceRadius;
    dst.shaderBlockID = static_cast<uint32_t>(src.shaderBlockID);
    dst.hairBetaM = src.hairBetaM;
    dst.hairBetaN = src.hairBetaN;
    dst.hairAlpha = src.hairAlpha;
}

void EditorDockSpaceAndMenuBar()
{
    // Semi-transparent window background
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));

    // DockSpace covers the full viewport; transparent background to show the backbuffer.
    ImGuiID dockspaceID = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

    // Set up default layout on first launch
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
        ImGui::DockBuilderDockWindow("Material", dockLeftBottom);
        ImGui::DockBuilderDockWindow("Camera", dockRight);
        ImGui::DockBuilderDockWindow("Lighting", dockRight);
        ImGui::DockBuilderDockWindow("Rendering", dockRight);
        ImGui::DockBuilderDockWindow("Profiler", dockRight);
        ImGui::DockBuilderFinish(dockspaceID);
    }

    // Main menu bar
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open Scene..."))
            {
                nfdu8filteritem_t filters[] = {{"Scene Files", "gltf,glb,fscn"}};
                nfdopendialogu8args_t args = {0};
                args.filterList = filters;
                args.filterCount = 1;
                nfdu8char_t* outPath = nullptr;
                if (NFD_OpenDialogU8_With(&outPath, &args) == NFD_OKAY)
                {
                    ReplaceScene(outPath);
                    NFD_FreePathU8(outPath);
                }
            }
            if (ImGui::MenuItem("Open HDR..."))
            {
                nfdu8filteritem_t filters[] = {{"HDR Images", "hdr,hdri,exr"}};
                nfdopendialogu8args_t args = {0};
                args.filterList = filters;
                args.filterCount = 1;
                nfdu8char_t* outPath = nullptr;
                if (NFD_OpenDialogU8_With(&outPath, &args) == NFD_OKAY)
                {
                    LoadEnvMap(outPath);
                    NFD_FreePathU8(outPath);
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S"))
            {
                if (!GEditor.doc.currentSavePath.empty())
                    SaveScene(GEditor.doc.currentSavePath);
            }
            if (ImGui::MenuItem("Save As..."))
            {
                nfdu8filteritem_t filters[] = {{"Scene Files", "fscn"}};
                nfdsavedialogu8args_t args = {0};
                args.filterList = filters;
                args.filterCount = 1;
                nfdu8char_t* outPath = nullptr;
                if (NFD_SaveDialogU8_With(&outPath, &args) == NFD_OKAY)
                {
                    SaveScene(outPath);
                    NFD_FreePathU8(outPath);
                }
            }
            ImGui::Separator();
            if (!GEditor.doc.instances.empty())
            {
                if (ImGui::MenuItem("Render HDR..."))
                {
                    // Two save filters -> NFD shows a dropdown. User-picked extension determines
                    // the encoder used by SaveHDR (ext-based dispatch).
                    nfdu8filteritem_t filters[] = {
                        {"Radiance HDR", "hdr"},
                        {"OpenEXR",      "exr"},
                    };
                    nfdsavedialogu8args_t args = {0};
                    args.filterList = filters;
                    args.filterCount = 2;
                    nfdu8char_t* outPath = nullptr;
                    if (NFD_SaveDialogU8_With(&outPath, &args) == NFD_OKAY)
                    {
                        GEditor.renderTask.outputPath = outPath;
                        GEditor.renderTask.format = ERenderFormat::HDR;
                        GEditor.renderTask.openRenderPopup = true;
                        NFD_FreePathU8(outPath);
                    }
                }
            }
            ImGui::EndMenu();
        }

        // Render Settings modal popup (opened after file dialog)
        if (GEditor.renderTask.openRenderPopup)
        {
            ImGui::OpenPopup("Render Settings");
            GEditor.renderTask.openRenderPopup = false;
        }
        if (ImGui::BeginPopupModal("Render Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const bool pathTracerRender = GEditor.rendererMode == ERendererMode::PathTracer;
            ImGui::Text("Configure %s HDR render:", pathTracerRender ? "path tracer" : "raster");
            ImGui::Text("Output: %s", GEditor.renderTask.outputPath.c_str());
            ImGui::Separator();
            if (pathTracerRender)
            {
                ImGui::InputInt("Samples / pixel", &GEditor.renderTask.samplePopupInput);
                if (GEditor.renderTask.samplePopupInput < 1)
                    GEditor.renderTask.samplePopupInput = 1;
                uint32_t dispatchFrames = PTDispatchesForPixelSamples(
                    GEditor.shaderGlobals, static_cast<uint32_t>(GEditor.renderTask.samplePopupInput));
                ImGui::Text("Current SPP: %s, dispatch frames: %u",
                            kPTSPPOptions[PTSPPOptionIndex(GEditor.shaderGlobals)].label, dispatchFrames);
            }
            else
            {
                ImGui::TextUnformatted("Raster export captures the next rendered frame.");
            }
            if (ImGui::Button("Start Render"))
            {
                GEditor.renderTask.targetSamples = pathTracerRender ? GEditor.renderTask.samplePopupInput : 1;
                GEditor.renderTask.renderPaused = false;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.state = FERendering;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Right-aligned PT / Raster toggle
        if (!GEditor.doc.instances.empty())
        {
            float btnW_PT = ImGui::CalcTextSize("######").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float btnW_R = ImGui::CalcTextSize("######").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float totalW = btnW_PT + btnW_R;
            float avail = ImGui::GetContentRegionAvail().x;
            if (avail > totalW)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - totalW);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            if (GEditor.rendererMode == ERendererMode::PathTracer)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            else
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            const char* labelPTPause[] = {"  PT  ", "", "PAUSED", ""};
            if (ImGui::Button(labelPTPause[GEditor.renderTask.renderPaused ? (SDL_GetTicks() >> 9 & 3) : 0],
                              ImVec2(btnW_PT, 0)))
            {
                if (GEditor.rendererMode != ERendererMode::PathTracer)
                {
                    GEditor.rendererMode = ERendererMode::PathTracer;
                    GEditor.state = FERunningEnter;
                    GEditor.renderTask.renderPaused = false;
                }
                else
                    GEditor.renderTask.renderPaused ^= 1;
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();

            if (GEditor.rendererMode == ERendererMode::Raster)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            else
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            if (ImGui::Button("RASTER"))
            {
                if (GEditor.rendererMode != ERendererMode::Raster)
                {
                    GEditor.rendererMode = ERendererMode::Raster;
                    GEditor.state = FERunningEnter;
                }
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        ImGui::EndMainMenuBar();
    }

    ImGui::PopStyleColor(); // WindowBg
}

void FHierarchyPanel()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Hierarchy"))
    {
        if (GEditor.doc.instances.empty() && GEditor.doc.curveInstances.empty())
        {
            ImGui::TextDisabled("No instances loaded");
        }
        else
        {
            ImGui::Text("%zu mesh instances, %zu curve instances", GEditor.doc.instances.size(), GEditor.doc.curveInstances.size());
            ImGui::Separator();
            for (size_t i = 0; i < GEditor.doc.instances.size(); i++)
            {
                auto& inst = GEditor.doc.instances[i];
                char label[128];
                snprintf(label, sizeof(label), "Instance %zu -- Mesh %u, Mat %u", i, inst.meshIndex,
                         inst.materialIndex);
                bool selected = (GEditor.doc.selectedInstance == static_cast<int>(i));
                if (ImGui::Selectable(label, selected))
                {
                    GEditor.doc.selectedInstance = static_cast<int>(i);
                    GEditor.doc.selectedCurveInstance = -1;
                    GEditor.doc.selectedMaterial = static_cast<int>(inst.materialIndex);
                    GEditor.doc.selectedLight = -1; // deselect light when selecting instance
                }
            }
            for (size_t i = 0; i < GEditor.doc.curveInstances.size(); i++)
            {
                auto& inst = GEditor.doc.curveInstances[i];
                char label[128];
                snprintf(label, sizeof(label), "Curve %zu -- Curve %u, Mat %u", i, inst.curveIndex,
                         inst.materialIndex);
                bool selected = (GEditor.doc.selectedCurveInstance == static_cast<int>(i));
                if (ImGui::Selectable(label, selected))
                {
                    GEditor.doc.selectedInstance = -1;
                    GEditor.doc.selectedCurveInstance = static_cast<int>(i);
                    GEditor.doc.selectedMaterial = static_cast<int>(inst.materialIndex);
                    GEditor.doc.selectedLight = -1;
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // Material panel for the selected instance
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Material"))
    {
        int materialIndex = GetSelectedMaterialIndex();
        if (IsMaterialIndexValid(materialIndex))
        {
            GEditor.doc.selectedMaterial = materialIndex;

            auto& material = GEditor.doc.scene.mMaterials[materialIndex];
            auto& gpuMaterial = GEditor.doc.materials[materialIndex];
            ImGui::Text("Material %d", materialIndex);
            if (IsSelectedInstanceValid())
                ImGui::Text("From Instance %d", GEditor.doc.selectedInstance);
            else if (IsSelectedCurveInstanceValid())
                ImGui::Text("From Curve %d", GEditor.doc.selectedCurveInstance);
            ImGui::Separator();

            bool changed = false;
            const char* shaderBlockLabels[] = {"Principled", "Hair"};
            int shaderBlock = static_cast<int>(material.shaderBlockID);
            if (ImGui::Combo("Shader Block", &shaderBlock, shaderBlockLabels, IM_ARRAYSIZE(shaderBlockLabels)))
            {
                material.shaderBlockID = static_cast<FMaterialShaderBlock>(shaderBlock);
                changed = true;
            }

            ImGui::SeparatorText("Principled");
            changed |= ImGui::ColorEdit4("Base Color", &material.baseColorFactor.x);
            changed |= ImHDRColorEdit("Emissive", reinterpret_cast<float3&>(material.emissiveFactor), material.emissiveFactor.w /* otherwise unused */);
            changed |= ImGui::SliderFloat("Metallic", &material.metallicFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Roughness", &material.roughnessFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Transmission", &material.transmissionFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("IOR", &material.ior, 1.0f, 3.0f, "%.3f");
            changed |= ImGui::SliderFloat("Specular", &material.specularFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::DragFloat3("Specular Color", &material.specularColorFactor.x, 0.01f, 0.0f, FLT_MAX, "%.3f");
            changed |= ImGui::SliderFloat("Anisotropy Strength", &material.anisotropyStrength, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::DragFloat("Anisotropy Rotation", &material.anisotropyRotation, 0.01f, -FLT_MAX, FLT_MAX, "%.3f rad");

            ImGui::SeparatorText("Hair");
            changed |= ImGui::SliderFloat("Beta M", &material.hairBetaM, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Beta N", &material.hairBetaN, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::DragFloat("Alpha", &material.hairAlpha, 0.1f, -20.0f, 20.0f, "%.2f deg");

            ImGui::SeparatorText("Subsurface");
            changed |= ImGui::SliderFloat("Weight", &material.subsurfaceFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::ColorEdit3("Color", &material.subsurfaceColor.x);
            changed |= ImGui::DragFloat3("Radius", &material.subsurfaceRadius.x, 0.001f, 0.0f, FLT_MAX, "%.4f");
            changed |= ImGui::SliderFloat("Scale", &material.subsurfaceScale, 0.0f, 1.0f, "%.4f");

            ImGui::SeparatorText("Textures");
            ImGui::Text("Base Color: %u", gpuMaterial.baseColorTexture);
            ImGui::Text("Emissive: %u", gpuMaterial.emissiveTexture);
            ImGui::Text("Metallic/Roughness: %u", gpuMaterial.metallicRoughnessTexture);
            ImGui::Text("Normal: %u", gpuMaterial.normalTexture);
            ImGui::Text("Transmission: %u", gpuMaterial.transmissionTexture);
            ImGui::Text("Specular: %u", gpuMaterial.specularTexture);
            ImGui::Text("Specular Color: %u", gpuMaterial.specularColorTexture);
            ImGui::Text("Anisotropy: %u", gpuMaterial.anisotropyTexture);

            if (changed)
            {
                SyncMaterialToGPU(static_cast<uint32_t>(materialIndex));
                CommitSceneToGPU(true);
            }
        }
        else if (IsSelectedInstanceValid())
        {
            ImGui::TextDisabled("Selected instance has invalid material index");
        }
        else if (IsSelectedCurveInstanceValid())
        {
            ImGui::TextDisabled("Selected curve has invalid material index");
        }
        else
        {
            ImGui::TextDisabled("No instance selected");
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // Inspector panel (Instance)
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Inspector"))
    {
        if (GEditor.doc.selectedInstance >= 0 && GEditor.doc.selectedInstance < static_cast<int>(GEditor.doc.scene.mInstances.size()))
        {
            auto& pi = GEditor.doc.scene.mInstances[GEditor.doc.selectedInstance];
            ImGui::Text("Instance %d", GEditor.doc.selectedInstance);
            ImGui::Separator();
            bool changed = false;
            changed |= ImGui::DragFloat3("Position", &pi.transform.transform.x, 0.01f);
            changed |= ImGui::DragFloat4("Rotation", &pi.transform.rotation.x, 0.001f);
            changed |= ImGui::DragFloat3("Scale", &pi.transform.scale.x, 0.01f);

            // -- Gizmo controls --
            ImGui::Separator();
            if (ImGui::RadioButton("Translate (G)", GEditor.gizmo.op == ImGuizmo::TRANSLATE))
                GEditor.gizmo.op = ImGuizmo::TRANSLATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Rotate (R)", GEditor.gizmo.op == ImGuizmo::ROTATE))
                GEditor.gizmo.op = ImGuizmo::ROTATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Scale (Q)", GEditor.gizmo.op == ImGuizmo::SCALE))
                GEditor.gizmo.op = ImGuizmo::SCALE;
            if (GEditor.gizmo.op != ImGuizmo::SCALE)
            {
                if (ImGui::RadioButton("Local", GEditor.gizmo.mode == ImGuizmo::LOCAL))
                    GEditor.gizmo.mode = ImGuizmo::LOCAL;
                ImGui::SameLine();
                if (ImGui::RadioButton("World", GEditor.gizmo.mode == ImGuizmo::WORLD))
                    GEditor.gizmo.mode = ImGuizmo::WORLD;
            }

            // Build model matrix (TRS -> mat4)
            mat4 modelMatrix = translate(mat4(1.0f), vec3(pi.transform.transform)) * mat4_cast(pi.transform.rotation) *
                glm::scale(mat4(1.0f), vec3(pi.transform.scale));

            // ImGuizmo rendering — only when no light is selected (mutual exclusion)
            if (GEditor.doc.selectedLight < 0 && GEditor.viewport.HasRect())
            {
                ImGuizmo::BeginFrame();
                ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
                ImVec2 viewportSize = GEditor.viewport.Size();
                ImGuizmo::SetRect(GEditor.viewport.contentMin.x, GEditor.viewport.contentMin.y,
                                  viewportSize.x, viewportSize.y);
                // Note: ImGuizmo uses column-major float[16], matching GLM mat4 memory layout
                if (ImGuizmo::Manipulate(&GEditor.camera.view[0][0], &GEditor.camera.proj[0][0], GEditor.gizmo.op, GEditor.gizmo.mode,
                                         &modelMatrix[0][0]))
                {
                    // Decompose back to TRS
                    float3 newTranslation;
                    quat newRotation;
                    float3 newScale;
                    Math::decompose(modelMatrix, newScale, newRotation, newTranslation);
                    pi.transform.transform = newTranslation;
                    pi.transform.rotation = newRotation;
                    pi.transform.scale = newScale;
                    changed = true;
                }
            }
            if (changed)
            {
                // Sync CPU scene to GPU-side data
                auto& inst = GEditor.doc.instances[GEditor.doc.selectedInstance];
                inst.transform = pi.transform.transform;
                inst.rotation = pi.transform.rotation;
                inst.scale = pi.transform.scale;
                CommitSceneToGPU(true);
            }
            ImGui::Separator();
            auto& inst = GEditor.doc.instances[GEditor.doc.selectedInstance];
            ImGui::Text("Mesh Offset: %u", inst.meshOffset);
            ImGui::Text("Material Index: %u", inst.materialIndex);
            ImGui::Text("Mesh Index: %u", inst.meshIndex);
        }
        else if (GEditor.doc.selectedCurveInstance >= 0 &&
                 GEditor.doc.selectedCurveInstance < static_cast<int>(GEditor.doc.scene.mCurveInstances.size()))
        {
            auto& pi = GEditor.doc.scene.mCurveInstances[GEditor.doc.selectedCurveInstance];
            ImGui::Text("Curve %d", GEditor.doc.selectedCurveInstance);
            ImGui::Separator();
            bool changed = false;
            changed |= ImGui::DragFloat3("Position", &pi.transform.transform.x, 0.01f);
            changed |= ImGui::DragFloat4("Rotation", &pi.transform.rotation.x, 0.001f);
            changed |= ImGui::DragFloat3("Scale", &pi.transform.scale.x, 0.01f);

            ImGui::Separator();
            if (ImGui::RadioButton("Translate (G)", GEditor.gizmo.op == ImGuizmo::TRANSLATE))
                GEditor.gizmo.op = ImGuizmo::TRANSLATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Rotate (R)", GEditor.gizmo.op == ImGuizmo::ROTATE))
                GEditor.gizmo.op = ImGuizmo::ROTATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Scale (Q)", GEditor.gizmo.op == ImGuizmo::SCALE))
                GEditor.gizmo.op = ImGuizmo::SCALE;
            if (GEditor.gizmo.op != ImGuizmo::SCALE)
            {
                if (ImGui::RadioButton("Local", GEditor.gizmo.mode == ImGuizmo::LOCAL))
                    GEditor.gizmo.mode = ImGuizmo::LOCAL;
                ImGui::SameLine();
                if (ImGui::RadioButton("World", GEditor.gizmo.mode == ImGuizmo::WORLD))
                    GEditor.gizmo.mode = ImGuizmo::WORLD;
            }

            mat4 modelMatrix = translate(mat4(1.0f), vec3(pi.transform.transform)) * mat4_cast(pi.transform.rotation) *
                glm::scale(mat4(1.0f), vec3(pi.transform.scale));

            if (GEditor.doc.selectedLight < 0 && GEditor.viewport.HasRect())
            {
                ImGuizmo::BeginFrame();
                ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
                ImVec2 viewportSize = GEditor.viewport.Size();
                ImGuizmo::SetRect(GEditor.viewport.contentMin.x, GEditor.viewport.contentMin.y,
                                  viewportSize.x, viewportSize.y);
                if (ImGuizmo::Manipulate(&GEditor.camera.view[0][0], &GEditor.camera.proj[0][0], GEditor.gizmo.op, GEditor.gizmo.mode,
                                         &modelMatrix[0][0]))
                {
                    float3 newTranslation;
                    quat newRotation;
                    float3 newScale;
                    Math::decompose(modelMatrix, newScale, newRotation, newTranslation);
                    pi.transform.transform = newTranslation;
                    pi.transform.rotation = newRotation;
                    pi.transform.scale = newScale;
                    changed = true;
                }
            }
            if (changed)
            {
                auto& inst = GEditor.doc.curveInstances[GEditor.doc.selectedCurveInstance];
                inst.transform = pi.transform.transform;
                inst.rotation = pi.transform.rotation;
                inst.scale = pi.transform.scale;
                CommitSceneToGPU(true);
            }
            ImGui::Separator();
            auto& inst = GEditor.doc.curveInstances[GEditor.doc.selectedCurveInstance];
            ImGui::Text("Curve Offset: %u", inst.curveOffset);
            ImGui::Text("Material Index: %u", inst.materialIndex);
            ImGui::Text("Curve Index: %u", inst.curveIndex);
        }
        else
        {
            ImGui::TextDisabled("No instance selected");
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void FLightingPanel()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Lighting"))
    {
        bool anyChanged = false;
        static const char* kLightTypeNames[] = {"Directional", "Point", "Spot", "Disk", "Rect"};
        static constexpr int kLightTypeCount = 5;

        // ---- Scene Lights ----
        if (ImGui::CollapsingHeader("Scene Lights", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Add light button
            if (GEditor.doc.scene.mLights.size() < kMaxSceneLights)
            {
                if (ImGui::Button("+ Add Light"))
                {
                    FLight newLight{};
                    newLight.type = FLightType::Directional;
                    newLight.color = {1, 1, 1};
                    newLight.power = 1.0f;
                    newLight.transform.rotation = quat(1, 0, 0, 0);
                    newLight.transform.scale = {1, 1, 1};
                    GEditor.doc.scene.mLights.emplace_back(newLight);
                    anyChanged = true;
                }
            }
            else
                ImGui::TextDisabled("Max %u lights reached", kMaxSceneLights);

            int removeIndex = -1;
            for (int i = 0; i < static_cast<int>(GEditor.doc.scene.mLights.size()); i++)
            {
                auto& light = GEditor.doc.scene.mLights[i];
                ImGui::PushID(i);

                char header[64];
                snprintf(header, sizeof(header), "Light %d (%s)", i, kLightTypeNames[static_cast<int>(light.type)]);
                bool isLightSelected = (GEditor.doc.selectedLight == i);
                ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_DefaultOpen;
                if (isLightSelected)
                    headerFlags |= ImGuiTreeNodeFlags_Selected;
                bool headerOpen = ImGui::CollapsingHeader(header, headerFlags);
                if (ImGui::IsItemClicked())
                {
                    GEditor.doc.selectedLight = i;
                    GEditor.doc.selectedInstance = -1; // deselect instance when selecting light
                    GEditor.doc.selectedCurveInstance = -1;
                    GEditor.doc.selectedMaterial = -1;
                }
                if (headerOpen)
                {
                    bool lightChanged = false;

                    // Type selector
                    int typeInt = static_cast<int>(light.type);
                    if (ImGui::Combo("Type", &typeInt, kLightTypeNames, kLightTypeCount))
                    {
                        light.type = static_cast<FLightType>(typeInt);
                        lightChanged = true;
                    }

                    // Color + Power
                    lightChanged |= ImHDRColorEdit("Color", light.color, light.power);

                    // Direction (Euler angles) for lights with orientation
                    bool hasDirection = (light.type == FLightType::Directional || light.type == FLightType::Spot ||
                                         light.type == FLightType::Disk || light.type == FLightType::Rect);
                    if (hasDirection)
                    {
                        // Decompose quaternion → Euler yaw/pitch (YXZ intrinsic order).
                        // Convention: default forward is (0,0,-1), Y-up.
                        // rotation = rotateY(yaw) * rotateX(pitch)
                        // Extract pitch and yaw directly from the quaternion to avoid
                        // direction-vector round-trip instabilities at the poles.
                        float sinP = 2.0f *
                            (light.transform.rotation.w * light.transform.rotation.x -
                             light.transform.rotation.y * light.transform.rotation.z);
                        sinP = std::clamp(sinP, -1.0f, 1.0f);
                        float pitch = degrees(std::asin(sinP));

                        float sinY = 2.0f *
                            (light.transform.rotation.w * light.transform.rotation.y +
                             light.transform.rotation.x * light.transform.rotation.z);
                        float cosY = 1.0f -
                            2.0f *
                                (light.transform.rotation.x * light.transform.rotation.x +
                                 light.transform.rotation.y * light.transform.rotation.y);
                        float yaw = degrees(std::atan2(sinY, cosY));

                        bool dirChanged = false;
                        dirChanged |= ImGui::SliderFloat("Pitch", &pitch, -90.0f, 90.0f, "%.1f deg");
                        dirChanged |= ImGui::SliderFloat("Yaw", &yaw, -180.0f, 180.0f, "%.1f deg");
                        if (dirChanged)
                        {
                            // Reconstruct quaternion directly: rotateY(yaw) * rotateX(pitch)
                            quat yawQ = angleAxis(radians(yaw), vec3(0, 1, 0));
                            quat pitchQ = angleAxis(radians(pitch), vec3(1, 0, 0));
                            light.transform.rotation = normalize(yawQ * pitchQ);
                            lightChanged = true;
                        }
                    }

                    // Position for positional lights
                    bool hasPosition = (light.type == FLightType::Point || light.type == FLightType::Spot ||
                                        light.type == FLightType::Disk || light.type == FLightType::Rect);
                    if (hasPosition)
                    {
                        lightChanged |= ImGui::DragFloat3("Position", &light.transform.transform.x, 0.1f);
                    }

                    // Range for Point and Spot
                    if (light.type == FLightType::Point || light.type == FLightType::Spot)
                    {
                        lightChanged |= ImGui::DragFloat("Range", &light.range, 0.1f, 0.0f, 1000.0f, "%.2f (0=inf)");
                    }

                    // Spot cone angles
                    if (light.type == FLightType::Spot)
                    {
                        float innerDeg = degrees(light.spotInnerConeAngle);
                        float outerDeg = degrees(light.spotOuterConeAngle);
                        bool coneChanged = false;
                        coneChanged |= ImGui::SliderFloat("Inner Cone", &innerDeg, 0.0f, outerDeg, "%.1f deg");
                        coneChanged |= ImGui::SliderFloat("Outer Cone", &outerDeg, innerDeg, 90.0f, "%.1f deg");
                        if (coneChanged)
                        {
                            light.spotInnerConeAngle = radians(innerDeg);
                            light.spotOuterConeAngle = radians(outerDeg);
                            lightChanged = true;
                        }
                    }

                    // Disk/Rect extents
                    if (light.type == FLightType::Disk || light.type == FLightType::Rect)
                    {
                        lightChanged |= ImGui::DragFloat("Width", &light.width, 0.01f, 0.001f, 100.0f, "%.3f");
                        lightChanged |= ImGui::DragFloat("Height", &light.height, 0.01f, 0.001f, 100.0f, "%.3f");
                    }

                    // Two-sided toggle for area lights
                    if (light.type == FLightType::Disk || light.type == FLightType::Rect)
                    {
                        lightChanged |= ImGui::Checkbox("Two-Sided", &light.twoSided);
                        ImGui::SameLine();
                        lightChanged |= ImGui::Checkbox("Normalize", &light.normalize);
                    }

                    if (ImGui::SmallButton("Remove"))
                        removeIndex = i;

                    if (lightChanged)
                        anyChanged = true;

                    ImGui::Separator();
                }
                ImGui::PopID();
            }

            if (removeIndex >= 0)
            {
                GEditor.doc.scene.mLights.erase(GEditor.doc.scene.mLights.begin() + removeIndex);
                // Fix up light selection after removal
                if (GEditor.doc.selectedLight == removeIndex)
                    GEditor.doc.selectedLight = -1;
                else if (GEditor.doc.selectedLight > removeIndex)
                    GEditor.doc.selectedLight--;
                anyChanged = true;
            }
        }

        // ---- Ambient / Environment ----
        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
        {
            anyChanged |= ImHDRColorEdit("Ambient", GEditor.shaderGlobals.ambientColor, GEditor.shaderGlobals.ambientPower);

            ImGui::Separator();
            bool hasEnv = GEditor.shaderGlobals.useEnvMap != 0u;
            ImGui::Text(hasEnv ? "HDRI Loaded" : "No HDRI");
            if (hasEnv)
            {
                bool envChanged = false;
                envChanged |= ImGui::SliderFloat("Env Scale", &GEditor.shaderGlobals.envMapScale, 0.0f, 10.0f, "%.3f",
                                                 ImGuiSliderFlags_Logarithmic);
                envChanged |= ImGui::SliderFloat("Azimuth Offset", &GEditor.shaderGlobals.envAzimuthOffset, -180.0f, 180.0f,
                                              "%.1f deg");
                bool envEnabled = GEditor.shaderGlobals.useEnvMap != 0u;
                if (ImGui::Checkbox("Enable Env Map", &envEnabled))
                {
                    GEditor.shaderGlobals.useEnvMap = envEnabled ? 1u : 0u;
                    envChanged = true;
                }
                if (envChanged)
                    anyChanged = true;
            }
            ImGui::TextDisabled("Drag & drop .hdr/.hdri/.exr to load");
        }

        if (anyChanged)
        {
            UpdateSceneLights();
            GEditor.shaderGlobals.ptAccumulatedFrames = 0;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // Draw light shape overlays and ImGuizmo manipulator
    DrawLightGizmos();
}

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
        GEditor.cameraUpdated |= ImGui::SliderFloat3("Cam Center", &GEditor.camera.center.x, -50.0f, 50.0f);
        GEditor.cameraUpdated |= ImGui::SliderFloat("Cam Radius", &GEditor.camera.radius, 0.0f, 100.0f);
        GEditor.cameraUpdated |= ImGui::SliderAngle("Cam FOV Y", &GEditor.camera.fovY);
        ImGui::SliderFloat("Exposure (EV)", &GEditor.shaderGlobals.camEV, -16.0f, 16.0f);
        ImGui::Separator();
        ImGui::SliderFloat("WASD Speed", &GEditor.camera.moveSpeed, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        GEditor.cameraUpdated |= ImGui::Checkbox("Enable DOF", &GEditor.aperture.dofEnabled);
        if (GEditor.aperture.dofEnabled)
        {
            GEditor.cameraUpdated |=
                ImGui::SliderFloat("F-Stop", &GEditor.aperture.fStop, 0.1f, 128.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            GEditor.cameraUpdated |= ImGui::SliderFloat("Sensor Height", &GEditor.aperture.sensorHeightMm, 1.0f, 100.0f,
                                                        "%.2f mm", ImGuiSliderFlags_Logarithmic);
            GEditor.cameraUpdated |= ImGui::SliderFloat("Focal Distance", &GEditor.shaderGlobals.focalDistance, 0.1f, 1000.0f, "%.3f",
                                                ImGuiSliderFlags_Logarithmic);
            int apertureBlades = static_cast<int>(GEditor.shaderGlobals.apertureBlades);
            if (ImGui::SliderInt("Blades", &apertureBlades, 0, 16))
            {
                GEditor.shaderGlobals.apertureBlades = static_cast<uint32_t>(apertureBlades);
                GEditor.cameraUpdated = true;
            }
            GEditor.cameraUpdated |= ImGui::SliderAngle("Rotation", &GEditor.shaderGlobals.apertureRotation, -180.0f, 180.0f,
                                                        "%.1f deg");
            GEditor.cameraUpdated |=
                ImGui::SliderFloat("Ratio", &GEditor.shaderGlobals.apertureRatio, 0.01f, 16.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
            float apertureRadiusMm =
                ApertureRadiusFromFStop(GEditor.aperture.fStop, GEditor.aperture.sensorHeightMm, GEditor.camera.fovY);
            ImGui::Text("Aperture Radius: %.3f mm", apertureRadiusMm);
            DrawAperturePreview(GEditor.shaderGlobals.apertureBlades, GEditor.shaderGlobals.apertureRotation,
                                GEditor.shaderGlobals.apertureRatio);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Rendering"))
    {
        bool changed = false;
        ImGui::SeparatorText("Display");
        if (ImGui::Checkbox("Enable HDR", &GContext->enableHDR))
        {
            GEditor.state = FERunningEnter;
            GEditor.shaderGlobals.enableHDR = GContext->enableHDR ? 1 : 0;
            changed = true;
        }
        if (GContext->enableHDR)
        {
            ImGui::SliderFloat("Paper White Nits", &GEditor.shaderGlobals.paperWhiteNits, 50.0f, 500.0f, "%.0f");
        }
        if (GEditor.rendererMode == ERendererMode::PathTracer)
        {
            ImGui::SeparatorText("Path Tracer");
            if (ImModalButton("Fast", 0, 3))
            {
                GEditor.shaderGlobals.ptMaxBouncesDiffuse = 4;
                GEditor.shaderGlobals.ptMaxBouncesSpecular = 4;
                GEditor.shaderGlobals.ptMaxBouncesTransmission = 12;
                GEditor.shaderGlobals.ptFireflyClamp = 1.0f;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            if (ImModalButton("Full", 1, 3))
            {
                GEditor.shaderGlobals.ptMaxBouncesDiffuse = 32;
                GEditor.shaderGlobals.ptMaxBouncesSpecular = 32;
                GEditor.shaderGlobals.ptMaxBouncesTransmission = 32;
                GEditor.shaderGlobals.ptFireflyClamp = 2.0f;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            if (ImModalButton("Über", 2, 3))
            {
                GEditor.shaderGlobals.ptMaxBouncesDiffuse = 32;
                GEditor.shaderGlobals.ptMaxBouncesSpecular = 32;
                GEditor.shaderGlobals.ptMaxBouncesTransmission = 32;
                GEditor.shaderGlobals.ptFireflyClamp = 100.0f;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::SeparatorText("Performance");
            const bool serSupported = GContext->device->GetCapabilities().shaderExecutionReordering;
            bool serEnabled = serSupported && GEditor.rendererConfig.ptShaderExecutionReordering;
            ImGui::BeginDisabled(!serSupported);
            if (ImGui::Checkbox("Shader Execution Reordering", &serEnabled))
            {
                GEditor.rendererConfig.ptShaderExecutionReordering = serEnabled;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            ImGui::EndDisabled();
            if (!serSupported)
                ImGui::TextDisabled("SER is not supported by this device.");
            ImGui::SeparatorText("Ray Bounce");
            ImGui::SliderInt("Diffuse", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesDiffuse), 0, 64);
            ImGui::SliderInt("Specular", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesSpecular), 0, 64);
            ImGui::SliderInt("Transmission", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesTransmission), 0, 64);
            ImGui::SeparatorText("Sampling");
            ImGui::SliderFloat("Max Energy", &GEditor.shaderGlobals.ptFireflyClamp, 1.0f, 100.0f, "%.1f");
            static int ptSPPIndex = 3; // 1/25 SPP
            ptSPPIndex = PTSPPOptionIndex(GEditor.shaderGlobals);
            if (ImGui::SliderInt("SPP", &ptSPPIndex, 0, kPTSPPOptionCount - 1,
                                 kPTSPPOptions[ptSPPIndex].label, ImGuiSliderFlags_AlwaysClamp))
                SetPTSPPOption(ptSPPIndex);
            const char* samplerItems[] = {"PCG (Independent)", "Sobol (Quasi-Monte Carlo)"};
            int ptSampler = static_cast<int>(GEditor.rendererConfig.ptSampler);
            if (ImGui::Combo("Sampler", &ptSampler, samplerItems, 2))
            {
                GEditor.rendererConfig.ptSampler = static_cast<uint32_t>(ptSampler);
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            const char* lightSamplerItems[] = { "Uniform", "Power" };
            if (ImGui::Combo("Light Sampler", reinterpret_cast<int*>(&GContext->gpuScene->mLightSamplerType), lightSamplerItems, 2))
            {
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
        }
        if (GEditor.rendererMode == ERendererMode::Raster)
        {
            ImGui::SeparatorText("Rasterizer");
            static float lodLogThreshold = 3;
            ImGui::SliderFloat("LOD ", &lodLogThreshold, 0, 8);
            GEditor.shaderGlobals.lodThreshold = std::pow(10.0f, -lodLogThreshold);
            {
                const char* items[] = {"Overdraw", "Meshlet", "Material ID"};
                const unsigned values[] = {kViewOverdraw, kViewMeshlet, kViewMaterialID};
                ImGui::SeparatorText("Debug View");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */);
            }
            {
                const char* items[] = {"RT Shadows"};
                const unsigned values[] = {kEnableRasterRTShadows};
                ImGui::SeparatorText("Options");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values);
            }
            {
                const char* items[] = {"Frustum", "Occlusion"};
                const unsigned values[] = {kCullFrustum, kCullOcclusion};
                ImGui::SeparatorText("Culling");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.cullFlags, items, values);
            }
        }
        if (GEditor.rendererMode == ERendererMode::PathTracer)
        {
            {
                const char* items[] = {"Diffuse Buffer", "Specular Buffer"};
                const unsigned values[] = {kViewAOVDiffuse, kViewAOVSpecular};
                ImGui::SeparatorText("AOV View");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */);
            }
        }
        if (GEditor.rendererMode == ERendererMode::Raster)
        {
            const char* items[] = {"Position", "BaseColor", "Normal"};
            const unsigned values[] = {kViewPosition, kViewBaseColor, kViewNormal};
            ImGui::SeparatorText("GBuffer View");
            changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */);
        }
        if (changed)
            GEditor.state = FERunningEnter;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    FLightingPanel();
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
                Allocator* frameScratch = GContext->editorFrameScratch ? GContext->editorFrameScratch.get() : GLOBAL_ALLOC;
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
                            .color = pass.queue == RHIDeviceQueueType::Graphics ? ImColor(1.0f, 0.5f, 0.0f, 1.0f)
                                                                                : ImColor(0.0f, 0.5f, 0.0f, 1.0f),
                        };
                        samples.emplace_back(std::move(sample));
                        while (histograms.size() <= i)
                            histograms.emplace_back(kHistogramSamples, GLOBAL_ALLOC);
                        histograms[i].push(sample.endTick - sample.startTick);
                    }
                    float presentTimingRes;
                    lanes = ImProfilerAssignLanes(samples, frameScratch);
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
                            Vector<unsigned> bins(frameScratch);
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

void FRendering(RendererHandles const& handles)
{
    auto* renderer = GContext->renderer;
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();

    bool cancelRendering = false;
    uint32_t targetFrames = GEditor.rendererMode == ERendererMode::PathTracer
        ? PTAccumulationStepTarget(GEditor.shaderGlobals, static_cast<uint32_t>(GEditor.renderTask.targetSamples))
        : static_cast<uint32_t>(GEditor.renderTask.targetSamples);
    // Full-width progress bar at the top
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
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize);
        uint32_t completedSamples = GEditor.rendererMode == ERendererMode::PathTracer
            ? PTCompletedPixelSamples(GEditor.shaderGlobals)
            : GEditor.shaderGlobals.ptAccumulatedFrames;
        float fraction = GEditor.renderTask.targetSamples > 0 ? static_cast<float>(completedSamples) /
                static_cast<float>(GEditor.renderTask.targetSamples)
                                                            : 0.0f;
        char overlay[128];
        snprintf(overlay, sizeof(overlay), "%d / %d %s", completedSamples,
                 GEditor.renderTask.targetSamples,
                 GEditor.rendererMode == ERendererMode::PathTracer ? "samples" : "frames");
        ImGui::ProgressBar(fraction, ImVec2(barW, barH), overlay);
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    // Cancel button at the bottom-right corner
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
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize);
        cancelRendering = ImGui::Button(cancelLabel, ImVec2(btnW, btnH));
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    GEditor.shaderGlobals.frameNumber = renderer->GetFrame();
    renderer->ExecuteFrame();
    renderer->EndExecute();
    GEditor.shaderGlobals.ptAccumulatedFrames += PTSamplesPerDispatch(GEditor.shaderGlobals);

    if (cancelRendering)
    {
        GEditor.state = FERunning;
    }
    else if (GEditor.shaderGlobals.ptAccumulatedFrames >= targetFrames)
    {
        if (handles.numHdrRT > 0u)
            DoHDRReadback(handles);
        GEditor.state = FERunning;
    }
}


// Project a world-space point to screen-space ImVec2
static ImVec2 WorldToScreen(vec3 worldPos, mat4 const& viewProj, ImVec2 displaySize)
{
    vec4 clip = viewProj * vec4(worldPos, 1.0f);
    if (clip.w <= 0.0f)
        return {-1e4f, -1e4f}; // behind camera — off-screen sentinel
    vec3 ndc = vec3(clip) / clip.w;
    return {
        GEditor.viewport.contentMin.x + (ndc.x * 0.5f + 0.5f) * displaySize.x,
        GEditor.viewport.contentMin.y + (-ndc.y * 0.5f + 0.5f) * displaySize.y // flip Y for screen coords
    };
}

// Draw a wireframe circle in world space via ImDrawList
static void DrawWireCircle(ImDrawList* dl, vec3 center, vec3 u, vec3 v, vec2 radius,
                           mat4 const& viewProj, ImVec2 displaySize, ImU32 color, float thickness,
                           int segments = 32)
{
    for (int i = 0; i < segments; i++)
    {
        float a0 = i * 6.2831853f / segments;
        float a1 = (i + 1) * 6.2831853f / segments;
        vec3 p0 = center + u * (cosf(a0) * radius.x) + v * (sinf(a0) * radius.y);
        vec3 p1 = center + u * (cosf(a1) * radius.x) + v * (sinf(a1) * radius.y);
        dl->AddLine(WorldToScreen(p0, viewProj, displaySize),
                    WorldToScreen(p1, viewProj, displaySize), color, thickness);
    }
}

// Draw a line between two world-space points
static void DrawWorldLine(ImDrawList* dl, vec3 a, vec3 b,
                          mat4 const& viewProj, ImVec2 displaySize, ImU32 color, float thickness)
{
    dl->AddLine(WorldToScreen(a, viewProj, displaySize),
                WorldToScreen(b, viewProj, displaySize), color, thickness);
}


static void DrawDirectionalOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));
    float len = 2.0f;

    // Main direction arrow
    DrawWorldLine(dl, pos, pos + dir * len, vp, ds, col, 2.0f);

    // Arrowhead: 3 lines from tip back
    float3 u, v;
    buildOrthonormalBasis(dir, u, v);
    vec3 tip = pos + dir * len;
    for (int i = 0; i < 3; i++)
    {
        float a = i * 2.0943951f; // 120° apart
        vec3 base = tip - dir * 0.3f + (u * cosf(a) + v * sinf(a)) * 0.15f;
        DrawWorldLine(dl, tip, base, vp, ds, col, 2.0f);
    }

    // Small sun-like circle at origin
    DrawWireCircle(dl, pos, u, v, vec2(0.15f), vp, ds, col, 1.5f, 16);
}

static void DrawPointOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    ImVec2 center = WorldToScreen(pos, vp, ds);
    if (center.x < -1e3f) return; // behind camera

    // Compute outer ring radius in pixels.
    // Only the center is projected; radius is derived analytically.
    float outerPx;
    if (light.range > 0.0f)
    {
        float dist = glm::length(pos - GEditor.camera.position);
        float pixelsPerUnit = (ds.y * 0.5f) / (dist * tanf(GEditor.camera.fovY * 0.5f));
        outerPx = std::max(light.range * pixelsPerUnit, 8.0f);
    }
    else
    {
        outerPx = 28.0f; // fixed screen-space size
    }

    // Concentric 2D rings — omnidirectional, no axis bias
    constexpr int kRings = 3;
    for (int i = 0; i < kRings; i++)
    {
        float t = static_cast<float>(i + 1) / kRings;
        float radius = outerPx * t;
        // Fade inner rings slightly
        ImU32 ringCol = (col & 0x00FFFFFF) | (static_cast<ImU32>((col >> 24) * (0.4f + 0.6f * t)) << 24);
        dl->AddCircle(center, radius, ringCol, 32, 1.5f);
    }

    // Small filled dot at center
    dl->AddCircleFilled(center, 3.0f, col, 12);
}

static void DrawSpotOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));
    float coneLen = (light.range > 0.0f) ? light.range : 3.0f;
    float outerR = coneLen * tanf(light.spotOuterConeAngle);

    float3 u, v;
    buildOrthonormalBasis(dir, u, v);
    vec3 tip = pos + dir * coneLen;

    // Base circle at cone end
    DrawWireCircle(dl, tip, u, v, vec2(outerR), vp, ds, col, 1.5f, 24);

    // 4 cone edge lines from apex to base
    for (int i = 0; i < 4; i++)
    {
        float a = i * 1.5707963f; // 90° apart
        vec3 base = tip + (u * cosf(a) + v * sinf(a)) * outerR;
        DrawWorldLine(dl, pos, base, vp, ds, col, 1.5f);
    }

    // Inner cone circle (if different from outer)
    if (light.spotInnerConeAngle > 0.001f)
    {
        float innerR = coneLen * tanf(light.spotInnerConeAngle);
        DrawWireCircle(dl, tip, u, v, vec2(innerR), vp, ds, col & 0x80FFFFFF, 1.0f, 24);
    }
}

static void DrawDiskOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));

    float3 u, v;
    buildOrthonormalBasis(dir, u, v);

    // Disk circle
    DrawWireCircle(dl, pos, u, v, vec2(light.width, light.height), vp, ds, col, 1.5f);

    // Normal arrow
    DrawWorldLine(dl, pos, pos + dir * std::max(light.width, light.height) * 1.5f, vp, ds, col, 2.0f);
}

static void DrawRectOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));

    float3 u, v;
    buildOrthonormalBasis(dir, u, v);

    // Rectangle corners (half-extents)
    vec3 corners[4] = {
        pos + u * light.width + v * light.height,
        pos - u * light.width + v * light.height,
        pos - u * light.width - v * light.height,
        pos + u * light.width - v * light.height,
    };
    for (int i = 0; i < 4; i++)
        DrawWorldLine(dl, corners[i], corners[(i + 1) % 4], vp, ds, col, 1.5f);

    // Normal arrow from center
    DrawWorldLine(dl, pos, pos + dir * 0.5f, vp, ds, col, 2.0f);
}


static void DrawLightGizmos()
{
    auto& lights = GEditor.doc.scene.mLights;
    if (lights.empty() || !GEditor.viewport.HasRect())
        return;

    ImVec2 displaySize = GEditor.viewport.Size();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    mat4 viewProj = GEditor.camera.proj * GEditor.camera.view;

    // -- Shape overlays for all lights --
    for (int i = 0; i < static_cast<int>(lights.size()); i++)
    {
        bool selected = (i == GEditor.doc.selectedLight);
        ImU32 color = selected ? IM_COL32(255, 200, 50, 255)   // gold for selected
                               : IM_COL32(255, 255, 100, 100); // dim yellow for others

        auto& light = lights[i];
        switch (light.type)
        {
        case FLightType::Directional: DrawDirectionalOverlay(light, viewProj, drawList, displaySize, color); break;
        case FLightType::Point:       DrawPointOverlay(light, viewProj, drawList, displaySize, color);       break;
        case FLightType::Spot:        DrawSpotOverlay(light, viewProj, drawList, displaySize, color);        break;
        case FLightType::Disk:        DrawDiskOverlay(light, viewProj, drawList, displaySize, color);        break;
        case FLightType::Rect:        DrawRectOverlay(light, viewProj, drawList, displaySize, color);        break;
        }
    }

    // -- ImGuizmo manipulator for the selected light --
    if (GEditor.doc.selectedLight < 0 || GEditor.doc.selectedLight >= static_cast<int>(lights.size()))
        return;

    auto& light = lights[GEditor.doc.selectedLight];
    bool hasPosition = (light.type != FLightType::Directional);

    // Build model matrix from light transform (no scale — lights don't scale)
    mat4 modelMatrix = translate(mat4(1.0f), vec3(light.transform.transform))
                     * mat4_cast(light.transform.rotation);

    ImGuizmo::BeginFrame();
    ImGuizmo::SetDrawlist(drawList);
    ImGuizmo::SetRect(GEditor.viewport.contentMin.x, GEditor.viewport.contentMin.y,
                      displaySize.x, displaySize.y);

    // Directional: rotate only. Others: translate + rotate (never scale).
    ImGuizmo::OPERATION op = hasPosition ? GEditor.gizmo.op : ImGuizmo::ROTATE;
    if (op == ImGuizmo::SCALE)
        op = ImGuizmo::TRANSLATE; // lights don't have meaningful uniform scale

    if (ImGuizmo::Manipulate(&GEditor.camera.view[0][0], &GEditor.camera.proj[0][0],
                             op, GEditor.gizmo.mode, &modelMatrix[0][0]))
    {
        float3 newTranslation;
        quat newRotation;
        float3 newScale;
        Math::decompose(modelMatrix, newScale, newRotation, newTranslation);
        light.transform.transform = newTranslation;
        light.transform.rotation = newRotation;
        // Sync to GPU
        UpdateSceneLights();
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
    }
}