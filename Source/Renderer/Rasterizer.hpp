#pragma once
#include "Renderer.hpp"

enum class RasterInjectionPoint : uint8_t
{
    AfterGBuffer,
    BeforeLighting,
    AfterLighting,
};

struct RasterFeatureContext
{
    Renderer* renderer{nullptr};
    RendererUBO* globals{nullptr};
    GPUScene* gpu{nullptr};
    RendererConfig const* cfg{nullptr};
    RHIExtent2D extent{0u, 0u};
    ResourceHandle globalUBO{kInvalidHandle};
    ResourceHandle primitiveBuffer{kInvalidHandle};
    ResourceHandle dynamicPrimitiveBuffer{kInvalidHandle};
    ResourceHandle instanceBuffer{kInvalidHandle};
    ResourceHandle materialBuffer{kInvalidHandle};
    ResourceHandle lightBuffer{kInvalidHandle};
    ResourceHandle tlas{kInvalidHandle};
    ResourceHandle gbuffer0{kInvalidHandle};
    ResourceHandle gbuffer1{kInvalidHandle};
    ResourceHandle gbuffer2{kInvalidHandle};
    ResourceHandle depth{kInvalidHandle};
    ResourceHandle instanceID{kInvalidHandle};
    ResourceHandle motionVectors{kInvalidHandle};
    ResourceHandle hiz{kInvalidHandle};
    ResourceHandle hizSampler{kInvalidHandle};
    ResourceHandle diffuse{kInvalidHandle};
    ResourceHandle specular{kInvalidHandle};
    ResourceHandle ambientOcclusion{kInvalidHandle};
};

using RasterEffectCallback = void (*)(RasterFeatureContext& ctx, void const* config);

struct RasterFeature
{
    int order{0};
    void const* config{nullptr};
    RasterInjectionPoint injectionPoint{RasterInjectionPoint::BeforeLighting};
    RasterEffectCallback callback{nullptr};
};

/**
 * @brief Builds the RenderGraph for the Raster renderer.
 * @note AS updates through (@ref BuildGPUSceneAccelerationStructureUpdatePass) is already conditionally built by this
 * pass
 */
extern void BuildRasterRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                   RendererConfig const& cfg, RendererOutputs& out, Span<const RasterFeature> features);
