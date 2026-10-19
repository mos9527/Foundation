#pragma once
#include "../Rasterizer.hpp"

using namespace Foundation;
using namespace Foundation::Core;
using namespace Foundation::RenderCore;

struct TAAConfig
{
    float historyWeight{0.9f};
    float jitterSpread{1.0f};
    uint32_t sampleCount{8u};
    bool neighborhoodClamp{true};
};

extern void TAAFeatureCallback(RasterFeatureContext& ctx, void const* configPtr);

inline RasterFeature TAAFeature(TAAConfig const* config)
{
    return RasterFeature{
        .order = 1000,
        .config = config,
        .injectionPoint = RasterInjectionPoint::AfterLighting,
        .callback = TAAFeatureCallback,
    };
}
