#pragma once
#include "../Examples.hpp"

bool Examples_RendererSwitchButton(ExampleInputState& input, ExampleRenderer& currentRenderer);
void Example_BuildExampleRasterRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                           RendererConfig& cfg, RendererOutputs& out);
void Example_BuildExampleProgressivePathTracerRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
												RendererConfig& cfg, RendererOutputs& out);
ResourceHandle Examples_BuildTonemappingPass(Renderer* renderer, RendererOutputs const& outputs, bool isPresent);

FImportedMesh Examples_MakePlaneMesh(float extent, float y = 0.0f, Allocator* alloc = GLOBAL_ALLOC);
FImportedMesh Examples_MakeBoxMesh(float size, Allocator* alloc = GLOBAL_ALLOC);
FImportedMesh Examples_MakeSphereMesh(float radius, uint32_t segments = 32, uint32_t rings = 16,
                                      Allocator* alloc = GLOBAL_ALLOC);
