#include "Matcap.hpp"

#include <algorithm>
#include <array>
#include "Tables/Matcaps.hpp"

namespace Matcap
{
namespace
{
template <size_t N>
constexpr std::array<Entry, N> ConvertEntries(::MatcapEntry const (&src)[N])
{
    std::array<Entry, N> dst{};
    for (size_t i = 0; i < N; ++i)
        dst[i] = {src[i].label, src[i].path};
    return dst;
}

std::array<Entry, kMatcapCount> const kEntries = ConvertEntries(kMatcaps);
} // namespace

Span<Entry const> EnumerateEntries()
{
    return Span<Entry const>(kEntries.data(), kEntries.size());
}

int GetDefaultEntryIndex()
{
    return kDefaultMatcap;
}

int GetExternalEntryIndex()
{
    return static_cast<int>(kEntries.size());
}

String ResolveSelectedPath(int& index, String const& externalPath)
{
    int const count = static_cast<int>(kEntries.size());
    int const externalIndex = GetExternalEntryIndex();
    if (index == externalIndex && !externalPath.empty())
        return externalPath;

    if (index < 0 || index >= externalIndex)
        index = std::clamp(GetDefaultEntryIndex(), 0, count - 1);
    return kEntries[index].path;
}

uint32_t ResolveTextureIndex(TextureHandle handle)
{
    return handle.IsValid() ? handle.index : UINT32_MAX;
}
} // namespace Matcap
