#include "Widgets.hpp"
using namespace EditorGlobals;
void OnImGui_RendererParamsWidget() {
    if (ImGui::Begin("Renderer")) {
        ImGui::Text("Frametime: %3f ms", ImGui::GetIO().DeltaTime * 1000);
    }
    ImGui::End();
}