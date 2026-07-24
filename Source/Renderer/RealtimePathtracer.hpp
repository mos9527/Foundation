#include "Renderer.hpp"

/**
 * @brief Builds the RenderGraph for the Realtime PT.
 * @note AS updates through (@ref BuildGPUSceneAccelerationStructureUpdatePass) is already conditionally built by this
 * pass
 */
extern void BuildRealtimePathTracerRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                       RendererConfig const& cfg, RendererOutputs& out);
