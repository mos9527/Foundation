#pragma once
#include <Core/Container.hpp>
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
    void* mData{};
    uint64_t mSize{};
    bool mWritable{};

#if defined(_WIN32)
    void* mFileHandle{};
    void* mMappingHandle{};
#else
    int mFd{-1};
#endif

    void MapView();
    void UnmapView() noexcept;
    void MoveFrom(MemoryMappedFile& other) noexcept;

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
    [[nodiscard]] bool IsMapped() const noexcept { return mData != nullptr; }
    [[nodiscard]] bool IsWritable() const noexcept { return mWritable; }
    [[nodiscard]] bool Empty() const noexcept { return mSize == 0; }
    [[nodiscard]] uint64_t Size() const noexcept { return mSize; }

    [[nodiscard]] const unsigned char* Data() const noexcept { return static_cast<const unsigned char*>(mData); }
    [[nodiscard]] unsigned char* MutableData();
    [[nodiscard]] Span<const unsigned char> Bytes() const;
    [[nodiscard]] Span<unsigned char> MutableBytes();
};

} // namespace Foundation::Core
