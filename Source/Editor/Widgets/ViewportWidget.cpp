#include "Widgets.hpp"
#include "../../../Dependencies/IconsFontAwesome6.h"
#include "../Processor/MeshPicking.hpp"
#include "../Scene/SceneView.hpp"
using namespace EditorGlobals;
static ImGuizmo::OPERATION gizmoOperation = ImGuizmo::TRANSLATE;
static ImVec2 viewportScreenPos;
static ImVec2 viewportSize;


static ImVec2 uv_to_cursor(float2 uv) {
    return ImVec2{uv.x, uv.y} *viewportSize;
};
static ImVec2 uv_to_pixel(float2 uv) {
    return viewportScreenPos + uv_to_cursor(uv);
};
static ImU32 col4_to_u32(float4 color) {
    return IM_COL32(color.x * 255, color.y * 255, color.z * 255, color.w * 255);
}
void Viewport_DirectionGizmo(SceneLightComponent* light, float length=1.0f) {
    auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
    float3 b1, b2;
    BuildOrthonormalBasis(light->get_direction_vector(), b1, b2);
    float3 p0 = light->get_global_translation();
    float3 p1 = light->get_global_translation() + light->get_direction_vector() * length;
    float2 u0 = camera->project_to_uv(p0);
    float2 u1 = camera->project_to_uv(p1);
    constexpr float triangleSize = 0.1f;
    float2 t0 = camera->project_to_uv(p1 - b1 * 0.5 * triangleSize);
    float2 t1 = camera->project_to_uv(p1 + b1 * 0.5 * triangleSize);
    float2 t2 = camera->project_to_uv(p1 + light->get_direction_vector() * triangleSize);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(uv_to_pixel(u0), uv_to_pixel(u1), 0xffffffff);
    draw_list->AddCircleFilled(uv_to_pixel(u0), 8.0f, 0xffffffff);    
    draw_list->AddTriangleFilled(uv_to_pixel(t0), uv_to_pixel(t1), uv_to_pixel(t2), 0xff00ffff);
}
void Viewport_Light_Point_Gizmo(SceneLightComponent* light) {
    auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
    float3 p0 = light->get_global_translation();
    float2 u0 = camera->project_to_uv(p0);
    float rSS = SphereScreenSpaceRadius(p0, light->spot_point_radius, camera->get_global_translation(), camera->fov);
    float rSSpx = rSS * std::max(g_Editor.viewport.width, g_Editor.viewport.height);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddCircleFilled(uv_to_pixel(u0), 8.0f, 0xffffffff);
    draw_list->AddCircle(uv_to_pixel(u0), rSSpx, 0xffffffff);
}
void Viewport_Light_Directional_Gizmo(SceneLightComponent* light) {
    Viewport_DirectionGizmo(light, 1.0f);
}
void Viewport_Light_Spot_Gizmo(SceneLightComponent* light) {
    Viewport_DirectionGizmo(light, cosf(light->spot_size_rad));
    auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
    float3 b1, b2, b3;
    BuildOrthonormalBasis(light->get_direction_vector(), b1, b2);
    b3 = light->get_direction_vector().Cross(camera->get_look_direction()); // Parallel to view 
    constexpr uint points = 36;
    float3 o = light->get_global_translation() + light->get_direction_vector() * cosf(light->spot_size_rad);
    ImVec2 outerPoints[points], innerPoints[points];
    float dotMin = 1, dotMax = 0;
    ImVec2 epMin{}, epMax{};
    for (uint i = 0; i < points; i++) {
        float rad = XM_2PI * i / points;
        float cosTheta = cosf(rad);
        float sinTheta = sinf(rad);
        float outerRadius = sinf(light->spot_size_rad);
        float innerRadius = sinf(light->spot_size_rad * (1 - light->spot_size_blend));
        float3 outerOffset = b1 * cosTheta * outerRadius + b2 * sinTheta * outerRadius;
        float dot = outerOffset.Dot(b3);
        float3 outerPoint = o + outerOffset;
        float3 innerPoint = o + b1 * cosTheta * innerRadius + b2 * sinTheta * innerRadius;
        outerPoints[i] = uv_to_pixel(camera->project_to_uv(outerPoint));
        innerPoints[i] = uv_to_pixel(camera->project_to_uv(innerPoint));
        // Finds the point with the min/max angle offset to the basis
        // created from LightDir x ViewDir.
        // This ensures the frustum always faces perpendicular to the camera and
        // the two frustum lines does not occlude each other.
        // ...similar to what Blender does
        if (dot < dotMin) {
            dotMin = dot;
            epMin = outerPoints[i];
        }
        if (dot > dotMax) {
            dotMax = dot;
            epMax = outerPoints[i];
        }
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddPolyline(outerPoints, points, col4_to_u32(light->color), ImDrawFlags_Closed, 1.0f);
    draw_list->AddPolyline(innerPoints, points, col4_to_u32(light->color), ImDrawFlags_Closed, 1.0f);
    draw_list->AddLine(uv_to_pixel(camera->project_to_uv(light->get_global_translation())), epMin, col4_to_u32(light->color), 1.0f);
    draw_list->AddLine(uv_to_pixel(camera->project_to_uv(light->get_global_translation())), epMax, col4_to_u32(light->color), 1.0f);
}
void Viewport_Light_AreaDisk_Gizmo(SceneLightComponent* light) {
    Viewport_DirectionGizmo(light);
    auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
    float3 b1, b2;
    BuildOrthonormalBasis(light->get_direction_vector(), b1, b2);
    constexpr uint points = 36;
    ImVec2 diskPoints[points];
    for (uint i = 0; i < points; i++) {
        float rad = XM_2PI * i / points;
        float cosTheta = cosf(rad);
        float sinTheta = sinf(rad);
        float3 p1 = cosTheta * light->area_quad_disk_extents.x * b1;
        float3 p2 = sinTheta * light->area_quad_disk_extents.y * b2;
        diskPoints[i] = uv_to_pixel(camera->project_to_uv(p1 + p2 + light->get_global_translation()));
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddPolyline(diskPoints, points, col4_to_u32(light->color), ImDrawFlags_Closed, 1.0f);
}
void Viewport_Light_AreaQuad_Gizmo(SceneLightComponent* light) {
    Viewport_DirectionGizmo(light);
    auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
    float3 b1, b2;
    BuildOrthonormalBasis(light->get_direction_vector(), b1, b2);
    float3 p1 = light->get_global_translation() - b1 * light->area_quad_disk_extents.x - b2 * light->area_quad_disk_extents.y;
    float3 p2 = light->get_global_translation() + b1 * light->area_quad_disk_extents.x - b2 * light->area_quad_disk_extents.y;
    float3 p3 = light->get_global_translation() + b1 * light->area_quad_disk_extents.x + b2 * light->area_quad_disk_extents.y;
    float3 p4 = light->get_global_translation() - b1 * light->area_quad_disk_extents.x + b2 * light->area_quad_disk_extents.y;
    float2 u1 = camera->project_to_uv(p1);
    float2 u2 = camera->project_to_uv(p2);
    float2 u3 = camera->project_to_uv(p3);
    float2 u4 = camera->project_to_uv(p4);    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 points[] = { uv_to_pixel(u1), uv_to_pixel(u2), uv_to_pixel(u3), uv_to_pixel(u4) };
    draw_list->AddPolyline(points, 4, col4_to_u32(light->color), ImDrawFlags_Closed, 1.0f);
}
void Viewport_Light_AreaLine_Gizmo(SceneLightComponent* light) {
    Viewport_DirectionGizmo(light);
    auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
    float3 b1, b2;
    BuildOrthonormalBasis(light->get_direction_vector(), b1, b2);
    float3 p1 = light->get_global_translation() + b2 * light->area_line_length / -2;
    float3 p2 = light->get_global_translation() + b2 * light->area_line_length / 2;    
    float2 u1 = camera->project_to_uv(p1);
    float2 u2 = camera->project_to_uv(p2);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddLine(uv_to_pixel(u1), uv_to_pixel(u2), col4_to_u32(light->color), light->area_line_radius);
}
void Viewport_Light_Gizmo(SceneLightComponent* light) {
    auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
    switch (light->lightType) {
    case SceneLightComponent::Point:
        Viewport_Light_Point_Gizmo(light);
        break;
    case SceneLightComponent::Directional:
        Viewport_Light_Directional_Gizmo(light);
        break;
    case SceneLightComponent::Spot:
        Viewport_Light_Spot_Gizmo(light);
        break;
    case SceneLightComponent::AreaLine:
        Viewport_Light_AreaLine_Gizmo(light);
        break;
    case SceneLightComponent::AreaQuad:
        Viewport_Light_AreaQuad_Gizmo(light);
        break;
    case SceneLightComponent::AreaDisk:
        Viewport_Light_AreaDisk_Gizmo(light);
        break;
    }    
}
void Viewport_Armature_Gizmo(SceneArmatureComponent* armature) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
    static uint active_bone = -1;
    auto dfs = [&](auto& dfs_,uint bone) -> void {
        uint parent = armature->get_parent_bone(bone);
        if (parent != armature->root) {
            matrix mParent = armature->get_global_bone_matrices()[parent] * armature->get_global_transform();
            matrix mChild = armature->get_global_bone_matrices()[bone] * armature->get_global_transform();
            float3 dir = mChild.Translation() - mParent.Translation();
            ImVec2 u1 = uv_to_pixel(camera->project_to_uv(mParent.Translation()));
            ImVec2 u2 = uv_to_pixel(camera->project_to_uv(mChild.Translation()));
            draw_list->AddLine(u1, u2, 0xFFFF00FF, 5.0f);
            ImGui::PushID(bone);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0,0,0,0 });
            auto p = uv_to_cursor(camera->project_to_uv(mChild.Translation()));
            ImGui::SetCursorPos(p);
            if (ImGui::Button(ICON_FA_BONE "###")) {
                active_bone = bone;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
        for (uint child : armature->get_child_bones(bone))
            dfs_(dfs_, child);
    };
    for (uint root : armature->get_root_bones()) {
        dfs(dfs, root);
    }
    if (active_bone != -1 && armature->boneInvMap.size() > active_bone){
        auto viewMatrix = camera->view;
        auto projectionMatrix = camera->projection;
        AffineTransform globalTransform = armature->get_global_bone_matrices()[active_bone] * armature->get_global_transform();
        ImGuizmo::Manipulate(
            (float*)&viewMatrix.m,
            (float*)&projectionMatrix.m,
            gizmoOperation,
            ImGuizmo::WORLD,
            (float*)&globalTransform.m
        );
        if (ImGuizmo::IsUsing()){
            uint parent = armature->get_parent_bone(active_bone);
            globalTransform = globalTransform * armature->get_global_transform().Invert();
            if (parent != armature->root)
                globalTransform = globalTransform * armature->get_global_bone_matrices()[parent].Invert();

            armature->localMatrices[active_bone] = globalTransform;
            armature->update();
        }
    }
}
void Viewport_Gizmo(SceneComponent* component) {
    // TRS
    if (ImGui::IsKeyPressed(ImGuiKey_T))
        gizmoOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R))
        gizmoOperation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_S))
        gizmoOperation = ImGuizmo::SCALE;
    // Per-type Gizmos
    bool disableTransform = false;
    switch (component->get_type())
    {
    case SceneComponentType::Light:
        Viewport_Light_Gizmo(static_cast<SceneLightComponent*>(component));
        break;
    case SceneComponentType::Armature:
        Viewport_Armature_Gizmo(static_cast<SceneArmatureComponent*>(component));
        disableTransform = true;
        break;
    default:
        break;
    }
    if (component->get_entity() == g_Scene.scene->graph->get_root())
        disableTransform = true;
    if (!disableTransform) {
        // Transfrom -> ImGuizmo    
        AffineTransform worldMatrix = component->get_global_transform();
        auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
        auto viewMatrix = camera->view;
        auto projectionMatrix = camera->projection;        
        ImGuizmo::Manipulate(
            (float*)&viewMatrix.m,
            (float*)&projectionMatrix.m,
            gizmoOperation,
            ImGuizmo::LOCAL,
            (float*)&worldMatrix.m
        );
        if (ImGuizmo::IsUsing()) {
            auto parent = g_Scene.scene->graph->parent_of(component->get_entity());
            auto parentTransform = g_Scene.scene->get_base<SceneComponent>(parent)->get_global_transform();
            AffineTransform localMatrix = worldMatrix * parentTransform.Invert();
            component->set_local_transform(localMatrix);
        }
    }
}
void OnImGui_ViewportWidget() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("Viewport", 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        viewportScreenPos = ImGui::GetCursorScreenPos();
        ImGuizmo::SetDrawlist();
        viewportSize = (ImGui::GetWindowContentRegionMax() - ImGui::GetWindowContentRegionMin());
        g_Editor.set_viewport_dimension(viewportSize.x, viewportSize.y);        
        ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, g_Editor.viewport.width, g_Editor.viewport.height);
        ImVec2 viewportMouse{ 0,0 };
        if (g_Editor.render.image) {
            ImGui::Image(g_Editor.render.image, viewportSize);
            viewportMouse = ImGui::GetMousePos() - ImGui::GetItemRectMin();
        }
        if (ImGui::IsItemHovered()) {
            if (!ImGuizmo::IsOver())
                g_Editor.viewport.state.transition(ViewportManipulationEvents::HoverWithoutGizmo);
            else
                g_Editor.viewport.state.transition(ViewportManipulationEvents::HoverWithGizmo);
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsKeyDown(ImGuiKey_LeftAlt))
                g_Editor.viewport.state.transition(ViewportManipulationEvents::MouseDown);
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle) || ImGui::IsKeyReleased(ImGuiKey_LeftAlt))
                g_Editor.viewport.state.transition(ViewportManipulationEvents::MouseRelease);
            if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
                g_Editor.viewport.state.transition(ViewportManipulationEvents::ShiftDown);
            if (ImGui::IsKeyReleased(ImGuiKey_LeftShift))
                g_Editor.viewport.state.transition(ViewportManipulationEvents::ShiftRelease);
        }
        else {
            g_Editor.viewport.state.transition(ViewportManipulationEvents::MouseLeave);
        }
        // Picking
        if (g_Editor.render.materialBuffer.first && g_Editor.render.materialBuffer.second) {
            if (ImGui::IsItemHovered() && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
                uint2 upos = { (UINT)viewportMouse.x, (UINT)viewportMouse.y };
                auto& selected = g_Editor.viewport.meshPicker->GetSelectedMaterialBufferAndRect(
                    g_Editor.render.materialBuffer.first,
                    g_Editor.render.materialBuffer.second, 
                    upos, 
                    { 1,1 }
                );
                if (!ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
                    // Unselect all.
                    for (auto& mesh : g_Scene.scene->storage<SceneStaticMeshComponent>()) mesh.set_selected(false);
                    for (auto& mesh : g_Scene.scene->storage<SceneSkinnedMeshComponent>()) mesh.set_selected(false);
                }
                // Select the picked mesh component
                for (uint selectedIndex : selected) {
                    auto mesh = SceneView::DecodeMeshInstanceIndex(g_Scene.scene, selectedIndex);
                    // Bring this instance to the editing window                
                    mesh->set_selected(true);
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        g_Editor.editingComponent = mesh->get_entity();
                    }
                }
            }
        }

        if (g_Scene.scene->valid<SceneComponentType>(g_Editor.editingComponent)) {
            SceneComponent* selectedComponent = g_Scene.scene->get_base<SceneComponent>(g_Editor.editingComponent);
            Viewport_Gizmo(selectedComponent);
        }
        auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
        // Glyphs for all non-mesh components
        for (auto& light : g_Scene.scene->storage<SceneLightComponent>()) {
            if (g_Editor.editingComponent == light.get_entity())
                continue;
            auto p = uv_to_cursor(camera->project_to_uv(light.get_global_translation()));
            ImGui::SetCursorPos(p);
            ImGui::PushID(entt::to_integral(light.get_entity()));
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ 0,0,0,0 });
                if (ImGui::Button(ICON_FA_LIGHTBULB "###")) {
                    g_Editor.editingComponent = light.get_entity();
                }
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
