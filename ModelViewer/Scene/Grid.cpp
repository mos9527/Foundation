#include "Grid.hpp"
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

    }
} // namespace ModelViewer
