#pragma once
#include "Renderer.hpp"

struct GTAOConfig
{
    float radiusPixels{36.0f};
    float radiusWorld{2.0f};
    float intensity{2.0f};
    float bias{0.06f};
    uint32_t directionCount{2u};
    uint32_t stepCount{6u};
};

RasterFeature GTAOFeature(GTAOConfig const* config);
