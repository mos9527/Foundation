#include "MemoryMapped.hpp"
#include <Core/Logging.hpp>
#include <limits>

namespace Foundation::Core {
namespace {

Platform::MmapAccess ToPlatformAccess(MemoryMappedAccess access)
{
    return access == MemoryMappedAccess::ReadWrite ? Platform::MmapAccess::ReadWrite
                                                   : Platform::MmapAccess::ReadOnly;
}

size_t CheckedSizeT(uint64_t value, const char* name)
{
    CHECK_MSG(value <= static_cast<uint64_t>(std::numeric_limits<size_t>::max()),
              "{} exceeds size_t range", name);
    return static_cast<size_t>(value);
}

} // namespace

MemoryMappedFile::MemoryMappedFile(StringView path, MemoryMappedAccess access)
{
    Open(path, access);
}

MemoryMappedFile::MemoryMappedFile(StringView path, uint64_t size)
{
    Create(path, size);
}

MemoryMappedFile::~MemoryMappedFile()
{
    Close();
}

MemoryMappedFile::MemoryMappedFile(MemoryMappedFile&& other) noexcept
{
    mState = other.mState;
    other.mState = {};
}

MemoryMappedFile& MemoryMappedFile::operator=(MemoryMappedFile&& other) noexcept
{
    if (this != &other)
    {
        Close();
        mState = other.mState;
        other.mState = {};
    }
    return *this;
}

void MemoryMappedFile::Open(StringView path, MemoryMappedAccess access)
{
    Platform::MmapOpen(mState, path, ToPlatformAccess(access));
}

void MemoryMappedFile::Create(StringView path, uint64_t size)
{
    Platform::MmapCreate(mState, path, size);
}

void MemoryMappedFile::OpenOrCreate(StringView path, uint64_t size)
{
    Platform::MmapOpenOrCreate(mState, path, size);
}

void MemoryMappedFile::Resize(uint64_t size)
{
    Platform::MmapResize(mState, size);
}

void MemoryMappedFile::Flush(uint64_t offset, uint64_t size) const
{
    Platform::MmapFlush(mState, offset, size);
}

void MemoryMappedFile::Close() noexcept
{
    Platform::MmapClose(mState);
}

bool MemoryMappedFile::IsOpen() const noexcept
{
    return Platform::MmapIsOpen(mState);
}

unsigned char* MemoryMappedFile::MutableData()
{
    CHECK_MSG(mState.writable, "Memory mapped file is read-only");
    return static_cast<unsigned char*>(mState.data);
}

Span<const unsigned char> MemoryMappedFile::Bytes() const
{
    return { Data(), CheckedSizeT(mState.size, "Memory mapped file size") };
}

Span<unsigned char> MemoryMappedFile::MutableBytes()
{
    return { MutableData(), CheckedSizeT(mState.size, "Memory mapped file size") };
}

} // namespace Foundation::Core
