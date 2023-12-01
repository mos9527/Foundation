#include "Widgets.hpp"
using namespace EditorGlobalContext;
static ImVec2 viewportScreenPos;
static ImVec2 viewportSize;
static ImVec2 uv_to_pixel(float2 uv) {
    return viewportScreenPos + ImVec2{uv.x, uv.y} *viewportSize; 
};
static ImU32 col4_to_u32(float4 color) {
    return IM_COL32(color.x * 255, color.y * 255, color.z * 255, color.w * 255);
}
void Viewport_Light_Spot_Gizmo(SceneLightComponent* light) {
    auto* camera = scene.scene->try_get<SceneCameraComponent>(viewport.camera);
    float3 b1, b2;
    BuildOrthonormalBasis(light->get_direction_vector(), b1, b2);
    constexpr uint points = 36;
    float3 o = light->get_global_translation() + light->get_direction_vector();
    ImVec2 outerPoints[points], innerPoints[points];
    for (uint i = 0; i < points; i++) {
        float rad = XM_2PI * i / points;
        float cosTheta = cosf(rad)/* length(direction_vector) */;
        float sinTheta = sinf(rad);
        float outerRadius = sinf(light->spot_size_rad);
        float innerRadius = sinf(light->spot_size_rad * (1 - light->spot_size_blend));
        float3 outerPoint = o + b1 * cosTheta * outerRadius + b2 * sinTheta * outerRadius;
        float3 innerPoint = o + b1 * cosTheta * innerRadius + b2 * sinTheta * innerRadius;
        outerPoints[i] = uv_to_pixel(camera->project_to_uv(outerPoint));
        innerPoints[i] = uv_to_pixel(camera->project_to_uv(innerPoint));
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddPolyline(outerPoints, points, col4_to_u32(light->color), ImDrawFlags_Closed, 1.0f);
    draw_list->AddPolyline(innerPoints, points, col4_to_u32(light->color), ImDrawFlags_Closed, 1.0f);
}
void Viewport_Light_AreaDisk_Gizmo(SceneLightComponent* light) {
    auto* camera = scene.scene->try_get<SceneCameraComponent>(viewport.camera);
    float3 b1, b2;
    BuildOrthonormalBasis(light->get_direction_vector(), b1, b2);
    constexpr uint points = 36;
    ImVec2 diskPoints[points];
    for (uint i = 0; i < points; i++) {
        float rad = XM_2PI * i / points;
        float cosTheta = cosf(rad);
        float sinTheta = sinf(rad);
        float3 p1 = cosTheta * light->area_quad_disk_extents.x * b1;
        float3 p2 = sinTheta * light->area_quad_disk_extents.x * b2;
        diskPoints[i] = uv_to_pixel(camera->project_to_uv(p1 + p2 + light->get_global_translation()));
    }
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddConvexPolyFilled(diskPoints, points, col4_to_u32(light->color));
}
void Viewport_Light_AreaQuad_Gizmo(SceneLightComponent* light) {
    auto* camera = scene.scene->try_get<SceneCameraComponent>(viewport.camera);
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
    draw_list->AddQuadFilled(uv_to_pixel(u1), uv_to_pixel(u2), uv_to_pixel(u3), uv_to_pixel(u4), col4_to_u32(light->color));
}
void Viewport_Light_AreaLine_Gizmo(SceneLightComponent* light) {
    auto* camera = scene.scene->try_get<SceneCameraComponent>(viewport.camera);
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
    auto* camera = scene.scene->try_get<SceneCameraComponent>(viewport.camera);
    // Draw a line for direction
    float3 p0 = light->get_global_translation();
    float3 p1 = light->get_global_translation() + light->get_direction_vector();
    float2 u0 = camera->project_to_uv(p0);
    float2 u1 = camera->project_to_uv(p1);    
    ImDrawList* draw_list = ImGui::GetWindowDrawList();    
    draw_list->AddLine(uv_to_pixel(u0),uv_to_pixel(u1), 0xffffffff);
    switch (light->lightType) {
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

void Viewport_Gizmo(SceneComponent* component) {
    static ImGuizmo::OPERATION gizmoOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_T))
        gizmoOperation = ImGuizmo::TRANSLATE;
    if (ImGui::IsKeyPressed(ImGuiKey_R))
        gizmoOperation = ImGuizmo::ROTATE;
    if (ImGui::IsKeyPressed(ImGuiKey_S))
        gizmoOperation = ImGuizmo::SCALE;

    AffineTransform worldMatrix = component->get_global_transform();
    auto* camera = scene.scene->try_get<SceneCameraComponent>(viewport.camera);
    auto viewMatrix = camera->view;
    auto projectionMatrix = camera->projection;
    AffineTransform deltaTransform;
    ImGuizmo::Manipulate(
        (float*)&viewMatrix.m,
        (float*)&projectionMatrix.m,
        gizmoOperation,
        ImGuizmo::WORLD,
        (float*)&worldMatrix.m,
        (float*)&deltaTransform.m
    );
    if (ImGuizmo::IsUsing()) {
        AffineTransform localMatrix = component->get_local_transform();
        localMatrix = localMatrix * deltaTransform;
        component->set_local_transform(localMatrix);
    }

    // Additional Gizmos
    switch (component->get_type())
    {
    case SceneComponentType::Light:
        Viewport_Light_Gizmo(static_cast<SceneLightComponent*>(component));
        break;
    default:
        break;
    }
}
void OnImGui_ViewportWidget() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("Viewport", 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        viewportScreenPos = ImGui::GetCursorScreenPos();
        ImGuizmo::SetDrawlist();
        viewportSize = (ImGui::GetWindowContentRegionMax() - ImGui::GetWindowContentRegionMin());
        viewport.width = viewportSize.x, viewport.height = viewportSize.y;
        ImGuizmo::SetRect(ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, viewport.width, viewport.height);
        ImVec2 viewportMouse{ 0,0 };
        if (render.renderer->r_frameBufferSRV) {
            ImGui::Image((ImTextureID)render.renderer->r_frameBufferSRV->descriptor.get_gpu_handle().ptr, viewportSize);
            viewportMouse = ImGui::GetMousePos() - ImGui::GetItemRectMin();
        }
        if (ImGui::IsItemHovered()) {
            if (!ImGuizmo::IsOver())
                viewport.state.transition(ViewportManipulationEvents::HoverWithoutGizmo);
            else
                viewport.state.transition(ViewportManipulationEvents::HoverWithGizmo);
        }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsKeyDown(ImGuiKey_LeftAlt))
            viewport.state.transition(ViewportManipulationEvents::MouseDown);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle) || ImGui::IsKeyReleased(ImGuiKey_LeftAlt))
            viewport.state.transition(ViewportManipulationEvents::MouseRelease);
        if (ImGui::IsKeyDown(ImGuiKey_LeftShift))
            viewport.state.transition(ViewportManipulationEvents::ShiftDown);
        if (ImGui::IsKeyReleased(ImGuiKey_LeftShift))
            viewport.state.transition(ViewportManipulationEvents::ShiftRelease);
        if (ImGui::IsItemHovered() && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
            uint2 upos = { (UINT)viewportMouse.x, (UINT)viewportMouse.y };
            auto& selected = editor.meshSelection->GetSelectedMaterialBufferAndRect(render.renderer->r_materialBufferTex, render.renderer->r_materialBufferSRV, upos, { 1,1 });            
            if (!ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
                // Unselect all.
                for (auto& mesh : scene.scene->storage<SceneMeshComponent>()) mesh.set_selected(false);
            }
            // Select the picked mesh component
            for (uint selectedIndex : selected) {
                auto entity = scene.scene->storage<SceneMeshComponent>().at(selectedIndex);
                auto& mesh = scene.scene->get<SceneMeshComponent>(entity);
                mesh.set_selected(true);
                // Bring this instance to the editing window                
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    editor.editingComponent = mesh.get_entity();
                }
            }
        }
        if (scene.scene->valid<SceneComponentType>(editor.editingComponent)) {
            SceneComponent* selectedComponent = scene.scene->get_base<SceneComponent>(editor.editingComponent);
            Viewport_Gizmo(selectedComponent);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
