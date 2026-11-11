#pragma once
#include "Renderer.hpp"

inline constexpr RHIResourceFormat kPTDenoiserGuideFormat = RHIResourceFormat::R16G16B16A16SignedFloat;
inline constexpr RHIResourceFormat kPTDenoiserDepthFormat = RHIResourceFormat::R32SignedFloat;

struct PTDenoiserGuides
{
    // World-space shading normal; alpha is perceptual roughness, or -1 for transmission.
    ResourceHandle normalRoughness{kInvalidHandle};
    ResourceHandle albedoMetallic{kInvalidHandle};
    // Distance from the sampled lens origin to the primary hit, in world units.
    // Zero marks background, directly visible emitters, and other unfilterable samples.
    ResourceHandle linearDepth{kInvalidHandle};
    // Camera-only history is not valid for dynamic geometry/textures; spatial filtering still works.
    bool temporalSupported{true};
};

// Temporally resolves and spatially filters linear-HDR AOVs; raw textures are never overwritten.
// History is independent of accumulatedFrames. Increment globals->ptDenoiseHistoryEpoch
// on scene edits or camera cuts; perspective pinhole cameras can otherwise retain history.
void BuildPTDenoisePasses(Renderer* renderer, RendererUBO const* globals, RendererConfig const& cfg,
                         PTDenoiserGuides const& guides, RendererOutputs& outputs);
