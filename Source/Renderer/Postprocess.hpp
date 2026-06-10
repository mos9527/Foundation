#pragma once

#include "GPUScene.hpp"
#include "Renderer.hpp"

namespace Postprocess
{
enum class ViewLUTDomain
{
    SDR,
    HDR,
};

struct ViewLUTEntry
{
    const char* label;
    const char* path;
};

[[nodiscard]] Span<ViewLUTEntry const> EnumerateViewLUTEntries(ViewLUTDomain domain);
[[nodiscard]] int GetDefaultViewLUTIndex(ViewLUTDomain domain);
[[nodiscard]] int GetExternalViewLUTIndex(ViewLUTDomain domain);
[[nodiscard]] uint32_t MatchViewLUTIndex(ViewLUTDomain domain, StringView view, StringView look, uint32_t defaultIndex);
[[nodiscard]] String ResolveSelectedViewLUTPath(ViewLUTDomain domain, int& index, String const& externalPath);

[[nodiscard]] RHIResourceFormat GetPostprocessOutputFormat(bool hdr);
[[nodiscard]] uint32_t ResolvePostprocessViewLutIndex(TextureHandle sdrViewLut, TextureHandle hdrViewLut,
                                                      bool useHdrViewLut);
} // namespace Postprocess
