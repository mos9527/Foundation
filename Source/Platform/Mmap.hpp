#pragma once
#include <Core/Container.hpp>
#include <cstdint>

namespace Foundation::Platform {

struct MmapState
{
    void* data{};
    uint64_t size{};
    bool writable{};
    std::intptr_t file{-1};
    void* mapping{};
};

enum class MmapAccess
{
    ReadOnly,
    ReadWrite,
};

void MmapOpen(MmapState& state, Core::StringView path, MmapAccess access);
void MmapCreate(MmapState& state, Core::StringView path, uint64_t size);
void MmapOpenOrCreate(MmapState& state, Core::StringView path, uint64_t size);
void MmapResize(MmapState& state, uint64_t size);
void MmapFlush(MmapState const& state, uint64_t offset, uint64_t size);
void MmapClose(MmapState& state) noexcept;
[[nodiscard]] bool MmapIsOpen(MmapState const& state) noexcept;

} // namespace Foundation::Platform
