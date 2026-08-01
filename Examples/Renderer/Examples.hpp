#pragma once
#include "../Examples.hpp"

enum class ExampleRenderer
{
    Raster,
    RealtimePT,
    ProgressivePT,
    Count
};

ENUM_NAME_CONV_BEGIN(ExampleRenderer)
ENUM_NAME(Raster)
ENUM_NAME(RealtimePT)
ENUM_NAME(ProgressivePT)
ENUM_NAME_CONV_END()

inline constexpr bool IsRaster(ExampleRenderer renderer)
{
    return renderer == ExampleRenderer::Raster;
}
inline constexpr bool IsPathTracer(ExampleRenderer renderer)
{
    return renderer == ExampleRenderer::RealtimePT || renderer == ExampleRenderer::ProgressivePT;
}
inline constexpr bool IsRealtimePT(ExampleRenderer renderer)
{
    return renderer == ExampleRenderer::RealtimePT;
}
inline constexpr bool IsProgressivePT(ExampleRenderer renderer)
{
    return renderer == ExampleRenderer::ProgressivePT;
}

bool Examples_RendererSwitchButton(ExampleInputState& input, ExampleRenderer& currentRenderer);

// Toggle buttons for a bitmask group (mirrors Editor ImBitmaskOptionPicker).
bool Examples_BitmaskOptionPicker(ExampleInputState& input, unsigned& value, const char* const* labels,
                                  const unsigned* masks, unsigned count, bool solo = false);
template <typename T, typename Mask, size_t N>
bool Examples_BitmaskOptionPicker(ExampleInputState& input, T& value, const char* (&labels)[N],
                                  const Mask (&masks)[N], bool solo = false)
{
    unsigned bits = static_cast<unsigned>(value);
    unsigned maskBits[N];
    for (size_t i = 0; i < N; ++i)
        maskBits[i] = static_cast<unsigned>(masks[i]);
    bool const changed = Examples_BitmaskOptionPicker(input, bits, labels, maskBits, N, solo);
    value = static_cast<T>(bits);
    return changed;
}

// Cycle Off + exclusive options for a solo bitmask group.
bool Examples_BitmaskCycleButton(ExampleInputState& input, StringView label, unsigned& value,
                                 const char* const* labels, const unsigned* masks, unsigned count);
template <typename T, typename Mask, size_t N>
bool Examples_BitmaskCycleButton(ExampleInputState& input, StringView label, T& value,
                                 const char* (&labels)[N], const Mask (&masks)[N])
{
    unsigned bits = static_cast<unsigned>(value);
    unsigned maskBits[N];
    for (size_t i = 0; i < N; ++i)
        maskBits[i] = static_cast<unsigned>(masks[i]);
    bool const changed = Examples_BitmaskCycleButton(input, label, bits, labels, maskBits, N);
    value = static_cast<T>(bits);
    return changed;
}

// Debug / option flag controls for the active example renderer (mirrors Editor flag panels).
bool Examples_RendererFlagsControls(ExampleInputState& input, ExampleRenderer renderer, RendererConfig& cfg);
void Example_BuildExampleRasterRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                           RendererConfig& cfg, RendererOutputs& out);
void Example_BuildExampleProgressivePathTracerRenderGraph(Renderer* renderer, RendererUBO* globals,
                                                          RendererResources& gpu, RendererConfig& cfg,
                                                          RendererOutputs& out);
void Example_BuildExampleRealtimePathtracerRenderGraph(Renderer* renderer, RendererUBO* globals,
                                                          RendererResources& gpu, RendererConfig& cfg,
                                                          RendererOutputs& out);
void Example_BuildExampleRenderer(ExampleRenderer renderer, Renderer* r, RendererUBO* globals, RendererResources& gpu,
                                  RendererConfig& cfg, RendererOutputs& out);

ResourceHandle Examples_BuildTonemappingPass(Renderer* renderer, RendererUBO const* globals,
                                             RendererOutputs const& outputs, bool isPresent);

FImportedMesh Examples_MakePlaneMesh(float extent, float y = 0.0f, Allocator* alloc = GLOBAL_ALLOC);
FImportedMesh Examples_MakeBoxMesh(float size, Allocator* alloc = GLOBAL_ALLOC);
FImportedMesh Examples_MakeSphereMesh(float radius, uint32_t segments = 32, uint32_t rings = 16,
                                      Allocator* alloc = GLOBAL_ALLOC);
