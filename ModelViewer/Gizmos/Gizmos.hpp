#pragma once
#include <Math/Math.hpp>
#include <Core/Core.hpp>
#include <Bindings/ImGui.hpp>

namespace ModelViewer
{
    using namespace Foundation;
    using namespace Core;
    using namespace Math;
    inline Tuple<ImVec2,ImVec2,ImDrawList*> getGizmoDrawOffsetRegionList()
    {
        ImVec2 offset = ImGui::GetWindowPos();
        ImVec2 region = ImGui::GetContentRegionAvail();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        return {offset, region, drawList};
    }
    IMGUI_API void drawGizmoCameraFrustum(
        mat4 viewProj,
        mat4 frustumViewProj /* req. non-inf zFar (zCull) - use @ref Camera::GetCullParams */,
        ImColor color = ImColor(1.0f,1.0f,1.0f,0.75f),
        float lineThickness = 1.0f
    );
}