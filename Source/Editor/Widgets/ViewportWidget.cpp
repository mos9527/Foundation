#include "Widgets.hpp"
using namespace EditorGlobalContext;

void OnImGui_ViewportWidget() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0,0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("Viewport", 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        auto viewportSize = (ImGui::GetWindowContentRegionMax() - ImGui::GetWindowContentRegionMin());
        viewport.width = viewportSize.x, viewport.height = viewportSize.y;
        ImVec2 viewportMouse{ 0,0 };
        if (render.renderer->r_frameBufferSRV) {
            ImGui::Image((ImTextureID)render.renderer->r_frameBufferSRV->descriptor.get_gpu_handle().ptr, viewportSize);
            viewportMouse = ImGui::GetMousePos() - ImGui::GetItemRectMin();
        }
        if (ImGui::IsItemHovered())
            viewport.state.transition(ViewportManipulationEvents::HoverWithoutGizmo);
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
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}
