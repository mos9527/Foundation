#include "Renderer.hpp"

/**
 * @brief Builds the RenderGraph for the Progressive PT.
 * @note AS updates through (@ref BuildGPUSceneAccelerationStructureUpdatePass) is already conditionally built by this
 * pass
 */
extern void BuildProgressivePathTracerRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                       RendererConfig const& cfg, RendererOutputs& out);
