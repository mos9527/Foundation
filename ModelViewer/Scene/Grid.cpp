#include "Grid.hpp"
#include "../../cmake-build-debug/_deps/imgui-src/imgui.h"

namespace ModelViewer
{
    Grid::Params Grid::GetParams(Camera const& camera) const
    {
        return {
            .camera = camera.GetParams(),
            .dimension = dimension, .width = width,
            .type = static_cast<uint>(type)
        };
    }
    void Grid::OnImGui()
    {
        ImGui::SliderInt("Dimension", reinterpret_cast<int*>(&dimension), 1, 1e5);
        ImGui::DragFloat("Width", &width, 0.01f, 0.1f, 100.0f);
        ImGui::Combo(
            "Grid Type", reinterpret_cast<int*>(&type),
            "Cartesian\0Radial\0\0");
    }
} // namespace ModelViewer
