#include "Renderer.hpp"

/**
 * @brief Builds the RenderGraph for the Pathtracer.
 * @note AS updates through (@ref BuildGPUSceneAccelerationStructureUpdatePass) is already conditionally built by this
 * pass
 */
extern void BuildPathTracerRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                       RendererConfig const& cfg, RendererOutputs& out);
