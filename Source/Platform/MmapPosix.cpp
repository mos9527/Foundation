#include "Mmap.hpp"
#include <Core/Logging.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Foundation::Platform {
namespace {

size_t CheckedSizeT(uint64_t value, const char* name)
{
    CHECK_MSG(value <= static_cast<uint64_t>(std::numeric_limits<size_t>::max()),
              "{} exceeds size_t range", name);
    return static_cast<size_t>(value);
}

Core::String LastSystemErrorString()
{
    return Core::String(std::strerror(errno));
}

uint64_t GetFileSize64(int fd)
{
    struct stat st{};
    CHECK_MSG(fstat(fd, &st) == 0, "fstat failed: {}", LastSystemErrorString());
    CHECK_MSG(st.st_size >= 0, "File size is negative");
    return static_cast<uint64_t>(st.st_size);
}

void SetFileSize64(int fd, uint64_t size)
{
    CHECK_MSG(size <= static_cast<uint64_t>(std::numeric_limits<off_t>::max()),
              "File size exceeds POSIX off_t range");
    CHECK_MSG(ftruncate(fd, static_cast<off_t>(size)) == 0, "ftruncate failed: {}", LastSystemErrorString());
}

uint64_t GetPageSize()
{
    long pageSize = sysconf(_SC_PAGESIZE);
    CHECK_MSG(pageSize > 0, "sysconf(_SC_PAGESIZE) failed: {}", LastSystemErrorString());
    return static_cast<uint64_t>(pageSize);
}

int ToFd(std::intptr_t file)
{
    return static_cast<int>(file);
}

void UnmapView(MmapState& state) noexcept
{
    if (state.data != nullptr)
    {
        munmap(state.data, static_cast<size_t>(state.size));
        state.data = nullptr;
    }
}

void MapView(MmapState& state)
{
    if (state.size == 0)
        return;

    int protection = PROT_READ | (state.writable ? PROT_WRITE : 0);
    int flags = MAP_SHARED;
    void* data = mmap(nullptr, CheckedSizeT(state.size, "Memory mapped file size"), protection, flags,
                      ToFd(state.file), 0);
    CHECK_MSG(data != MAP_FAILED, "mmap failed: {}", LastSystemErrorString());
    state.data = data;
}

} // namespace

void MmapOpen(MmapState& state, Core::StringView path, MmapAccess access)
{
    MmapClose(state);
    state.writable = access == MmapAccess::ReadWrite;
    Core::String pathString(path);

    int flags = state.writable ? O_RDWR : O_RDONLY;
    int fd = open(pathString.c_str(), flags);
    CHECK_MSG(fd >= 0, "open failed for {}: {}", pathString, LastSystemErrorString());
    state.file = fd;
    try
    {
        state.size = GetFileSize64(fd);
        MapView(state);
    }
    catch (...)
    {
        MmapClose(state);
        throw;
    }
}

void MmapCreate(MmapState& state, Core::StringView path, uint64_t size)
{
    MmapClose(state);
    state.writable = true;
    state.size = size;
    Core::String pathString(path);

    int fd = open(pathString.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    CHECK_MSG(fd >= 0, "open failed for {}: {}", pathString, LastSystemErrorString());
    state.file = fd;
    try
    {
        SetFileSize64(fd, size);
        MapView(state);
    }
    catch (...)
    {
        MmapClose(state);
        throw;
    }
}

void MmapOpenOrCreate(MmapState& state, Core::StringView path, uint64_t size)
{
    MmapClose(state);
    state.writable = true;
    Core::String pathString(path);

    int fd = open(pathString.c_str(), O_RDWR | O_CREAT, 0666);
    CHECK_MSG(fd >= 0, "open failed for {}: {}", pathString, LastSystemErrorString());
    state.file = fd;
    try
    {
        uint64_t currentSize = GetFileSize64(fd);
        if (currentSize != size)
            SetFileSize64(fd, size);
        state.size = size;
        MapView(state);
    }
    catch (...)
    {
        MmapClose(state);
        throw;
    }
}

void MmapResize(MmapState& state, uint64_t size)
{
    CHECK_MSG(MmapIsOpen(state), "Cannot resize a closed memory mapped file");
    CHECK_MSG(state.writable, "Cannot resize a read-only memory mapped file");
    if (state.size == size)
        return;

    MmapFlush(state, 0, std::numeric_limits<uint64_t>::max());
    UnmapView(state);
    SetFileSize64(ToFd(state.file), size);
    state.size = size;
    MapView(state);
}

void MmapFlush(MmapState const& state, uint64_t offset, uint64_t size)
{
    if (!state.writable || state.data == nullptr || state.size == 0)
        return;

    CHECK_MSG(offset <= state.size, "Memory mapped flush offset is out of bounds");
    uint64_t flushSize = size == std::numeric_limits<uint64_t>::max() ? state.size - offset : size;
    CHECK_MSG(flushSize <= state.size - offset, "Memory mapped flush range is out of bounds");
    if (flushSize == 0)
        return;

    uint64_t pageSize = GetPageSize();
    uint64_t alignedOffset = offset - offset % pageSize;
    uint64_t alignmentDelta = offset - alignedOffset;
    uint64_t alignedFlushSize = flushSize + alignmentDelta;
    unsigned char* alignedBase = static_cast<unsigned char*>(state.data) + alignedOffset;
    CHECK_MSG(msync(alignedBase, CheckedSizeT(alignedFlushSize, "Memory mapped flush size"), MS_SYNC) == 0,
              "msync failed: {}", LastSystemErrorString());
}

void MmapClose(MmapState& state) noexcept
{
    UnmapView(state);
    if (state.file >= 0)
    {
        close(ToFd(state.file));
        state.file = -1;
    }
    state.size = 0;
    state.writable = false;
}

bool MmapIsOpen(MmapState const& state) noexcept
{
    return state.file >= 0;
}

} // namespace Foundation::Platform
