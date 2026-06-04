#include "Postprocess.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include "Tables/ViewLUTs.hpp"

namespace Postprocess
{
namespace
{
template <size_t N>
constexpr std::array<ViewLUTEntry, N> ConvertEntries(::ViewLUTEntry const (&src)[N])
{
    std::array<ViewLUTEntry, N> dst{};
    for (size_t i = 0; i < N; ++i)
        dst[i] = {src[i].label, src[i].path};
    return dst;
}

std::string_view Trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

Pair<std::string_view, std::string_view> SplitViewLUTLabel(std::string_view label)
{
    size_t split = label.find(" / ");
    if (split == std::string_view::npos)
        return {Trim(label), std::string_view{}};
    return {Trim(label.substr(0, split)), Trim(label.substr(split + 3))};
}

bool ViewLUTLabelGreaterEqual(Pair<std::string_view, std::string_view> const& lhs,
                              Pair<std::string_view, std::string_view> const& rhs)
{
    int viewCompare = lhs.first.compare(rhs.first);
    if (viewCompare != 0)
        return viewCompare > 0;
    return lhs.second.compare(rhs.second) >= 0;
}

bool ViewLUTLabelLess(Pair<std::string_view, std::string_view> const& lhs,
                      Pair<std::string_view, std::string_view> const& rhs)
{
    int viewCompare = lhs.first.compare(rhs.first);
    if (viewCompare != 0)
        return viewCompare < 0;
    return lhs.second.compare(rhs.second) < 0;
}

std::array<ViewLUTEntry, kViewLUTSdrCount> const kEntriesSdr = ConvertEntries(kViewLUTsSdr);
std::array<ViewLUTEntry, kViewLUTHdrCount> const kEntriesHdr = ConvertEntries(kViewLUTsHdr);
} // namespace

Span<ViewLUTEntry const> EnumerateViewLUTEntries(ViewLUTDomain domain)
{
    if (domain == ViewLUTDomain::HDR)
        return Span<ViewLUTEntry const>(kEntriesHdr.data(), kEntriesHdr.size());
    return Span<ViewLUTEntry const>(kEntriesSdr.data(), kEntriesSdr.size());
}

int GetDefaultViewLUTIndex(ViewLUTDomain domain)
{
    return domain == ViewLUTDomain::HDR ? kDefaultViewLUTHdr : kDefaultViewLUTSdr;
}

int GetExternalViewLUTIndex(ViewLUTDomain domain)
{
    return static_cast<int>(EnumerateViewLUTEntries(domain).size());
}

uint32_t MatchViewLUTIndex(ViewLUTDomain domain, StringView view, StringView look, uint32_t defaultIndex)
{
    if (view.empty())
        return defaultIndex;

    Span<ViewLUTEntry const> entries = EnumerateViewLUTEntries(domain);
    Optional<uint32_t> viewNoLook;
    Optional<uint32_t> lexicographic;
    Pair<std::string_view, std::string_view> lexicographicLabel;
    Pair<std::string_view, std::string_view> target{view, look};

    for (uint32_t i = 0; i < entries.size(); ++i)
    {
        Pair<std::string_view, std::string_view> candidate = SplitViewLUTLabel(entries[i].label);
        if (candidate.first == view && candidate.second == look)
            return i;
        if (candidate.first == view && candidate.second == "No Look")
            viewNoLook = i;
        if (ViewLUTLabelGreaterEqual(candidate, target) &&
            (!lexicographic.has_value() || ViewLUTLabelLess(candidate, lexicographicLabel)))
        {
            lexicographic = i;
            lexicographicLabel = candidate;
        }
    }

    if (viewNoLook.has_value())
        return *viewNoLook;
    if (lexicographic.has_value())
        return *lexicographic;
    return defaultIndex;
}

String ResolveSelectedViewLUTPath(ViewLUTDomain domain, int& index, String const& externalPath)
{
    Span<ViewLUTEntry const> entries = EnumerateViewLUTEntries(domain);
    int const defaultIndex = GetDefaultViewLUTIndex(domain);
    int const externalIndex = static_cast<int>(entries.size());
    if (index == externalIndex && !externalPath.empty())
        return externalPath;

    if (index < 0 || index >= externalIndex)
        index = std::clamp(defaultIndex, 0, externalIndex - 1);
    return entries[index].path;
}

RHIResourceFormat GetPostprocessOutputFormat()
{
    return RHIResourceFormat::A2B10G10R10Unorm;
}

uint32_t ResolvePostprocessViewLutIndex(TextureHandle sdrViewLut, TextureHandle hdrViewLut, bool useHdrViewLut)
{
    TextureHandle const selected = useHdrViewLut ? hdrViewLut : sdrViewLut;
    return selected.IsValid() ? selected.index : UINT32_MAX;
}
} // namespace Postprocess
