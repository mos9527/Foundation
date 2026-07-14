#pragma once
#include "Renderer.hpp"

struct RasterGTAOConfig
{
    float radiusPixels{36.0f};
    float radiusWorld{2.0f};
    float intensity{2.0f};
    float bias{0.06f};
    uint32_t directionCount{2u};
    uint32_t stepCount{6u};
};

struct RasterMotionBlurConfig
{
    float intensity{4.0f};
    uint32_t sampleCount{8u};
    float maximumVelocity{200.0f}; // pixels
    float minimumVelocity{2.0f};   // pixels
    float depthComparisonExtent{1.0f};
};

RasterEffect MakeRasterGTAOEffect(RasterGTAOConfig const* config);
RasterEffect MakeRasterMotionBlurEffect(RasterMotionBlurConfig const* config);
