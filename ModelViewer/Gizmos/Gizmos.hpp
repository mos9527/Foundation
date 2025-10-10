#pragma once
#include <Math/Math.hpp>
#include <Core/Core.hpp>
#include <imgui.h>
namespace ModelViewer
{
    using namespace Foundation;
    using namespace Core;
    using namespace Math;
    inline Pair<ImVec2,ImDrawList*> gizmoGetCurrentRegionAndDrawList()
    {
        // TODO: Change these once we're doing dynamic viewport window within ImGui
        ImVec2 region = ImGui::GetIO().DisplaySize;
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();
        return {region, drawList};
    }
    IMGUI_API void gizmosDrawCameraFrustum(
        mat4 viewProj,
        mat4 frustumViewProj,
        ImColor color = ImColor(1.0f,1.0f,1.0f,0.75f),
        float lineThickness = 1.0f
    );
}