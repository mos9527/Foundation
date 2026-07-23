#include "../Rasterizer.hpp"
#include <Core/Paths.hpp>

using namespace Foundation;
using namespace Foundation::Core;
using namespace Foundation::RenderCore;

struct GTAOConfig
{
    float radiusPixels{36.0f};
    float radiusWorld{2.0f};
    float intensity{2.0f};
    float bias{0.06f};
    uint32_t directionCount{2u};
    uint32_t stepCount{6u};
};
extern void GTAOFeatureCallback(RasterFeatureContext& ctx, void const* configPtr);

inline RasterFeature GTAOFeature(GTAOConfig const* config)
{
    return RasterFeature{
        .order = 0,
        .config = config,
        .injectionPoint = RasterInjectionPoint::BeforeLighting,
        .callback = GTAOFeatureCallback,
    };
}
