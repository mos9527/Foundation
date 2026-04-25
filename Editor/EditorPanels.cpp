#include <nfd.h>
#include <Math/Decompose.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include <imgui_internal.h>
#include "EditorState.hpp"

/* ==================== DockSpace + Menu Bar ==================== */
void EditorDockSpaceAndMenuBar()
{
    // Semi-transparent window background
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));

    // DockSpace covers the full viewport; transparent background to show the backbuffer
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
            if (GEditor.rendererMode == ERendererMode::PathTracer && !GEditor.doc.instances.empty())
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
            ImGui::Text("Configure HDR render:");
            ImGui::Text("Output: %s", GEditor.renderTask.outputPath.c_str());
            ImGui::Separator();
            ImGui::InputInt("Samples (frames)", &GEditor.renderTask.samplePopupInput);
            if (GEditor.renderTask.samplePopupInput < 1)
                GEditor.renderTask.samplePopupInput = 1;
            if (ImGui::Button("Start Render"))
            {
                GEditor.renderTask.targetSamples = GEditor.renderTask.samplePopupInput;
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

/* ==================== Hierarchy Panel ==================== */
void FHierarchyPanel()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Hierarchy"))
    {
        if (GEditor.doc.instances.empty())
        {
            ImGui::TextDisabled("No instances loaded");
        }
        else
        {
            ImGui::Text("%zu instances", GEditor.doc.instances.size());
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
                    GEditor.doc.selectedLight = -1; // deselect light when selecting instance
                }
            }
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
            if (GEditor.doc.selectedLight < 0)
            {
                ImGuizmo::BeginFrame();
                ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
                auto& io = ImGui::GetIO();
                ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
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
                // Re-upload instance array to GPU
                auto* gpu = GContext->gpuScene;
                auto res = gpu->UpdateGPUScene(GEditor.doc.instances, GEditor.doc.materials, GEditor.doc.lights);
                GEditor.shaderGlobals.firstInstance = res.firstInstance;
                GEditor.shaderGlobals.firstMaterial = res.firstMaterial;
                GEditor.shaderGlobals.firstLight = res.firstLight;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::Separator();
            auto& inst = GEditor.doc.instances[GEditor.doc.selectedInstance];
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

/* ==================== Lighting Panel ==================== */
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

/* ==================== Running ImGui (Camera, Rendering, Profiler) ==================== */
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
        GEditor.cameraUpdated |=
            ImGui::SliderFloat("Aperture", &GEditor.shaderGlobals.aperture, 1e-5f, 1.0f, "%.5f", ImGuiSliderFlags_Logarithmic);
        GEditor.cameraUpdated |= ImGui::SliderFloat("Focal Distance", &GEditor.shaderGlobals.focalDistance, 0.1f, 1000.0f, "%.3f",
                                            ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Exposure (EV)", &GEditor.shaderGlobals.camEV, -16.0f, 16.0f);
        ImGui::Separator();
        ImGui::SliderFloat("WASD Speed", &GEditor.camera.moveSpeed, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
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
            ImGui::SeparatorText("Ray Bounce");
            ImGui::SliderInt("Diffuse", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesDiffuse), 0, 64);
            ImGui::SliderInt("Specular", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesSpecular), 0, 64);
            ImGui::SliderInt("Transmission", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesTransmission), 0, 64);
            ImGui::SeparatorText("Sampling");
            ImGui::SliderFloat("Max Energy", &GEditor.shaderGlobals.ptFireflyClamp, 1.0f, 100.0f, "%.1f");
            const char* samplerItems[] = {"PCG (Independent)", "Sobol (Quasi-Monte Carlo)"};
            if (ImGui::Combo("Sampler", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptSampler), samplerItems, 2))
            {
                GEditor.shaderGlobals.ptAccumulatedFrames = 0; // Reset accumulation on sampler change
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

/* ==================== HDR Readback ==================== */
void DoHDRReadback(PTReadbackHandles const& handles)
{
    auto* renderer = GContext->renderer;
    auto [w, h] = renderer->GetSwapchainExtent();
    const size_t pixelCount = static_cast<size_t>(w) * h;
    const size_t imageBytes = pixelCount * 4 * sizeof(float); // RGBA32F

    auto* diffuseTex = renderer->DerefResource(handles.diffuse).Get<RHITexture*>();
    auto* specularTex = renderer->DerefResource(handles.specular).Get<RHITexture*>();

    auto readbackBuf = GContext->device->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Readback,
                                                                    .hostAccess = RHIResourceHostAccess::ReadWrite,
                                                                    .coherent = true},
                                                       .usage = RHIBufferUsageBits::TransferDestination,
                                                       .size = imageBytes * 2});

    {
        ImmediateContext ctx(RHIDeviceQueueType::Graphics, GContext->device.Get());
        auto* cmd = ctx.Get();
        cmd->Begin();
        cmd->BeginTransition();
        cmd->SetImageTransition(diffuseTex,
                                {.srcAccess = RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
                                 .dstAccess = RHIResourceAccessBits::TransferRead,
                                 .srcStage = RHIPipelineStageBits::BottomOfPipe,
                                 .dstStage = RHIPipelineStageBits::Transfer,
                                 .srcImgLayout = RHITextureLayout::General,
                                 .dstImgLayout = RHITextureLayout::TransferSrc,
                                 .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->SetImageTransition(specularTex,
                                {.srcAccess = RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
                                 .dstAccess = RHIResourceAccessBits::TransferRead,
                                 .srcStage = RHIPipelineStageBits::BottomOfPipe,
                                 .dstStage = RHIPipelineStageBits::Transfer,
                                 .srcImgLayout = RHITextureLayout::General,
                                 .dstImgLayout = RHITextureLayout::TransferSrc,
                                 .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->EndTransition();
        cmd->CopyImageToBuffer(
            diffuseTex, RHITextureLayout::TransferSrc, readbackBuf.Get(),
            {{{.dstBufferOffset = 0, .srcLayer = {.aspect = RHITextureAspectFlagBits::Color}, .extent = {w, h, 1}}}});
        cmd->CopyImageToBuffer(specularTex, RHITextureLayout::TransferSrc, readbackBuf.Get(),
                               {{{.dstBufferOffset = static_cast<uint32_t>(imageBytes),
                                  .srcLayer = {.aspect = RHITextureAspectFlagBits::Color},
                                  .extent = {w, h, 1}}}});
        cmd->BeginTransition();
        cmd->SetImageTransition(diffuseTex,
                                {.srcAccess = RHIResourceAccessBits::TransferRead,
                                 .dstAccess = RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
                                 .srcStage = RHIPipelineStageBits::Transfer,
                                 .dstStage = RHIPipelineStageBits::TopOfPipe,
                                 .srcImgLayout = RHITextureLayout::TransferSrc,
                                 .dstImgLayout = RHITextureLayout::General,
                                 .srcImgRange = {.layer = {.aspect = RHITextureAspectFlagBits::Color}, .mipCount = 1}});
        cmd->SetImageTransition(specularTex,
                                {.srcAccess = RHIResourceAccessBits::TransferRead,
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
    const float* diffuseData = mapped;
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

    const char* hdrPath =
        GEditor.renderTask.outputPath.empty() ? "render_output.hdr" : GEditor.renderTask.outputPath.c_str();
    SaveHDR(combined.data(), static_cast<int>(w), static_cast<int>(h), hdrPath);
    LOG(Editor, LogInfo, "HDR image saved to {} ({}x{}, {} samples)", hdrPath, w, h,
        GEditor.shaderGlobals.ptAccumulatedFrames);
}

/* ==================== FRendering (offline render loop) ==================== */
void FRendering(PTReadbackHandles const& handles)
{
    auto* renderer = GContext->renderer;
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();

    bool cancelRendering = false;
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
        float fraction = GEditor.renderTask.targetSamples > 0 ? static_cast<float>(GEditor.shaderGlobals.ptAccumulatedFrames) /
                static_cast<float>(GEditor.renderTask.targetSamples)
                                                            : 0.0f;
        char overlay[128];
        snprintf(overlay, sizeof(overlay), "%d / %d samples", GEditor.shaderGlobals.ptAccumulatedFrames,
                 GEditor.renderTask.targetSamples);
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
    GEditor.shaderGlobals.ptAccumulatedFrames++;

    if (cancelRendering)
    {
        GEditor.state = FERunning;
    }
    else if (GEditor.shaderGlobals.ptAccumulatedFrames >= static_cast<uint32_t>(GEditor.renderTask.targetSamples))
    {
        if (handles.diffuse != kInvalidHandle && handles.specular != kInvalidHandle)
            DoHDRReadback(handles);
        GEditor.state = FERunning;
    }
}