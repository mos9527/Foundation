#pragma once

#include "GPUScene.hpp"

namespace Matcap
{
struct Entry
{
    const char* label;
    const char* path;
};

[[nodiscard]] Span<Entry const> EnumerateEntries();
[[nodiscard]] int GetDefaultEntryIndex();
[[nodiscard]] int GetExternalEntryIndex();
[[nodiscard]] String ResolveSelectedPath(int& index, String const& externalPath);
[[nodiscard]] uint32_t ResolveTextureIndex(TextureHandle handle);
} // namespace Matcap
