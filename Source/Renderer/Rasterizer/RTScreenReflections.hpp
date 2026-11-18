#pragma once
#include "../Rasterizer.hpp"

// Allocated only for the inline-RT path. Lighting writes these before any reflection/TAA composition.
struct RasterReflectionResources
{
    ResourceHandle radiance{kInvalidHandle}; // Raw half-res lighting; SPD sampling source for GGX prefilter
    ResourceHandle environmentSpecular{kInvalidHandle};
    uint32_t mipLevels{0u};
};

RasterReflectionResources CreateRasterReflectionResources(Renderer* renderer, RHIExtent2D extent);
void BuildRasterRTScreenReflections(RasterFeatureContext& ctx, RasterReflectionResources resources);
