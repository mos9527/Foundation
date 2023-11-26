#include "Widgets.hpp"
using namespace EditorGlobalContext;
void OnImGui_RendererWidget() {
    if (ImGui::Begin("Renderer")) {
        ImGui::Text("Frametime: %3f ms", ImGui::GetIO().DeltaTime * 1000);
        static bool debug_ViewAlbedo = false, debug_ViewLod = false, debug_Wireframe = false;
        static bool debug_FrustumCull = true, debug_OcclusionCull = true;
        ImGui::Checkbox("View LOD", &debug_ViewLod);
        ImGui::Checkbox("View Albedo", &debug_ViewAlbedo);
        ImGui::Checkbox("Wireframe", &debug_Wireframe);
        ImGui::Checkbox("Frustum Cull", &debug_FrustumCull);
        ImGui::Checkbox("Occlusion Cull", &debug_OcclusionCull);
        uint frameFlags = 0;
        if (debug_ViewAlbedo) frameFlags |= FRAME_FLAG_DEBUG_VIEW_ALBEDO;
        if (debug_ViewLod) frameFlags |= FRAME_FLAG_DEBUG_VIEW_LOD;
        if (debug_Wireframe) frameFlags |= FRAME_FLAG_WIREFRAME;
        if (debug_FrustumCull) frameFlags |= FRAME_FLAG_FRUSTUM_CULL;
        if (debug_OcclusionCull) frameFlags |= FRAME_FLAG_OCCLUSION_CULL;
        viewport.frameFlags = frameFlags;
        ImGui::End();
    }
}