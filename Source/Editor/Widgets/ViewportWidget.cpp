#include "Widgets.hpp"
using namespace EditorGlobalContext;

void OnImGui_ViewportWidget() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("Viewport", 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        auto viewportSize = (ImGui::GetWindowContentRegionMax() - ImGui::GetWindowContentRegionMin());
        viewport.width = viewportSize.x, viewport.height = viewportSize.y;
        if (viewport.frame)
            ImGui::Image((ImTextureID)viewport.frame->descriptor.get_gpu_handle().ptr, viewportSize);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}