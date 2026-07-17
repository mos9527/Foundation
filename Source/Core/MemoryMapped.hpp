#pragma once
#include <Platform/Mmap.hpp>
#include <cstdint>
#include <limits>

namespace Foundation::Core {

enum class MemoryMappedAccess
{
    ReadOnly,
    ReadWrite,
};

class MemoryMappedFile
{
    Platform::MmapState mState{};

public:
    MemoryMappedFile() = default;
    MemoryMappedFile(StringView path, MemoryMappedAccess access);
    MemoryMappedFile(StringView path, uint64_t size);
    ~MemoryMappedFile();

    MemoryMappedFile(MemoryMappedFile const&) = delete;
    MemoryMappedFile& operator=(MemoryMappedFile const&) = delete;

    MemoryMappedFile(MemoryMappedFile&& other) noexcept;
    MemoryMappedFile& operator=(MemoryMappedFile&& other) noexcept;

    void Open(StringView path, MemoryMappedAccess access);
    void Create(StringView path, uint64_t size);
    void OpenOrCreate(StringView path, uint64_t size);
    void Resize(uint64_t size);
    void Flush(uint64_t offset = 0, uint64_t size = std::numeric_limits<uint64_t>::max()) const;
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept;
    [[nodiscard]] bool IsMapped() const noexcept { return mState.data != nullptr; }
    [[nodiscard]] bool IsWritable() const noexcept { return mState.writable; }
    [[nodiscard]] bool Empty() const noexcept { return mState.size == 0; }
    [[nodiscard]] uint64_t Size() const noexcept { return mState.size; }

    [[nodiscard]] const unsigned char* Data() const noexcept { return static_cast<const unsigned char*>(mState.data); }
    [[nodiscard]] unsigned char* MutableData();
    [[nodiscard]] Span<const unsigned char> Bytes() const;
    [[nodiscard]] Span<unsigned char> MutableBytes();
};

} // namespace Foundation::Core
