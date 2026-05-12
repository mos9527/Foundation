#include "MemoryMapped.hpp"
#include <Core/Logging.hpp>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Foundation::Core {

namespace {

size_t CheckedSizeT(uint64_t value, const char* name)
{
    CHECK_MSG(value <= static_cast<uint64_t>(std::numeric_limits<size_t>::max()),
              "{} exceeds size_t range", name);
    return static_cast<size_t>(value);
}

#if defined(_WIN32)

HANDLE ToHandle(void* handle)
{
    return static_cast<HANDLE>(handle);
}

void* FromHandle(HANDLE handle)
{
    return static_cast<void*>(handle);
}

String LastSystemErrorString()
{
    DWORD error = GetLastError();
    if (error == 0)
        return {};

    LPSTR message = nullptr;
    DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                      FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                  reinterpret_cast<LPSTR>(&message), 0, nullptr);
    String result;
    if (length != 0 && message != nullptr)
        result.assign(message, length);
    else
        result = fmt::format("Windows error {}", error);
    if (message != nullptr)
        LocalFree(message);
    return result;
}

uint64_t GetFileSize64(HANDLE file)
{
    LARGE_INTEGER size{};
    CHECK_MSG(GetFileSizeEx(file, &size) != 0, "GetFileSizeEx failed: {}", LastSystemErrorString());
    CHECK_MSG(size.QuadPart >= 0, "File size is negative");
    return static_cast<uint64_t>(size.QuadPart);
}

void SetFileSize64(HANDLE file, uint64_t size)
{
    CHECK_MSG(size <= static_cast<uint64_t>(std::numeric_limits<LONGLONG>::max()),
              "File size exceeds Windows LARGE_INTEGER range");
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(size);
    CHECK_MSG(SetFilePointerEx(file, distance, nullptr, FILE_BEGIN) != 0,
              "SetFilePointerEx failed: {}", LastSystemErrorString());
    CHECK_MSG(SetEndOfFile(file) != 0, "SetEndOfFile failed: {}", LastSystemErrorString());
}

#else

String LastSystemErrorString()
{
    return String(std::strerror(errno));
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

#endif

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
    MoveFrom(other);
}

MemoryMappedFile& MemoryMappedFile::operator=(MemoryMappedFile&& other) noexcept
{
    if (this != &other)
    {
        Close();
        MoveFrom(other);
    }
    return *this;
}

void MemoryMappedFile::MoveFrom(MemoryMappedFile& other) noexcept
{
    mData = other.mData;
    mSize = other.mSize;
    mWritable = other.mWritable;

#if defined(_WIN32)
    mFileHandle = other.mFileHandle;
    mMappingHandle = other.mMappingHandle;
    other.mFileHandle = nullptr;
    other.mMappingHandle = nullptr;
#else
    mFd = other.mFd;
    other.mFd = -1;
#endif

    other.mData = nullptr;
    other.mSize = 0;
    other.mWritable = false;
}

void MemoryMappedFile::Open(StringView path, MemoryMappedAccess access)
{
    Close();
    mWritable = access == MemoryMappedAccess::ReadWrite;
    String pathString(path);

#if defined(_WIN32)
    DWORD desiredAccess = mWritable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD shareMode = FILE_SHARE_READ | (mWritable ? 0 : FILE_SHARE_WRITE);
    HANDLE file = CreateFileA(pathString.c_str(), desiredAccess, shareMode, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK_MSG(file != INVALID_HANDLE_VALUE, "CreateFileA failed for {}: {}", pathString, LastSystemErrorString());
    mFileHandle = FromHandle(file);
    try
    {
        mSize = GetFileSize64(file);
        MapView();
    }
    catch (...)
    {
        Close();
        throw;
    }
#else
    int flags = mWritable ? O_RDWR : O_RDONLY;
    int fd = open(pathString.c_str(), flags);
    CHECK_MSG(fd >= 0, "open failed for {}: {}", pathString, LastSystemErrorString());
    mFd = fd;
    try
    {
        mSize = GetFileSize64(fd);
        MapView();
    }
    catch (...)
    {
        Close();
        throw;
    }
#endif
}

void MemoryMappedFile::Create(StringView path, uint64_t size)
{
    Close();
    mWritable = true;
    mSize = size;
    String pathString(path);

#if defined(_WIN32)
    HANDLE file = CreateFileA(pathString.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK_MSG(file != INVALID_HANDLE_VALUE, "CreateFileA failed for {}: {}", pathString, LastSystemErrorString());
    mFileHandle = FromHandle(file);
    try
    {
        SetFileSize64(file, size);
        MapView();
    }
    catch (...)
    {
        Close();
        throw;
    }
#else
    int fd = open(pathString.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    CHECK_MSG(fd >= 0, "open failed for {}: {}", pathString, LastSystemErrorString());
    mFd = fd;
    try
    {
        SetFileSize64(fd, size);
        MapView();
    }
    catch (...)
    {
        Close();
        throw;
    }
#endif
}

void MemoryMappedFile::OpenOrCreate(StringView path, uint64_t size)
{
    Close();
    mWritable = true;
    String pathString(path);

#if defined(_WIN32)
    HANDLE file = CreateFileA(pathString.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK_MSG(file != INVALID_HANDLE_VALUE, "CreateFileA failed for {}: {}", pathString, LastSystemErrorString());
    mFileHandle = FromHandle(file);
    try
    {
        uint64_t currentSize = GetFileSize64(file);
        if (currentSize != size)
            SetFileSize64(file, size);
        mSize = size;
        MapView();
    }
    catch (...)
    {
        Close();
        throw;
    }
#else
    int fd = open(pathString.c_str(), O_RDWR | O_CREAT, 0666);
    CHECK_MSG(fd >= 0, "open failed for {}: {}", pathString, LastSystemErrorString());
    mFd = fd;
    try
    {
        uint64_t currentSize = GetFileSize64(fd);
        if (currentSize != size)
            SetFileSize64(fd, size);
        mSize = size;
        MapView();
    }
    catch (...)
    {
        Close();
        throw;
    }
#endif
}

void MemoryMappedFile::Resize(uint64_t size)
{
    CHECK_MSG(IsOpen(), "Cannot resize a closed memory mapped file");
    CHECK_MSG(mWritable, "Cannot resize a read-only memory mapped file");
    if (mSize == size)
        return;

    Flush();
    UnmapView();

#if defined(_WIN32)
    SetFileSize64(ToHandle(mFileHandle), size);
#else
    SetFileSize64(mFd, size);
#endif

    mSize = size;
    MapView();
}

void MemoryMappedFile::MapView()
{
    if (mSize == 0)
        return;

#if defined(_WIN32)
    DWORD protect = mWritable ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE mapping = CreateFileMappingA(ToHandle(mFileHandle), nullptr, protect, 0, 0, nullptr);
    CHECK_MSG(mapping != nullptr, "CreateFileMappingA failed: {}", LastSystemErrorString());
    mMappingHandle = FromHandle(mapping);

    DWORD desiredAccess = mWritable ? FILE_MAP_WRITE : FILE_MAP_READ;
    mData = MapViewOfFile(mapping, desiredAccess, 0, 0, 0);
    CHECK_MSG(mData != nullptr, "MapViewOfFile failed: {}", LastSystemErrorString());
#else
    int protection = PROT_READ | (mWritable ? PROT_WRITE : 0);
    int flags = MAP_SHARED;
    void* data = mmap(nullptr, CheckedSizeT(mSize, "Memory mapped file size"), protection, flags, mFd, 0);
    CHECK_MSG(data != MAP_FAILED, "mmap failed: {}", LastSystemErrorString());
    mData = data;
#endif
}

void MemoryMappedFile::UnmapView() noexcept
{
#if defined(_WIN32)
    if (mData != nullptr)
    {
        UnmapViewOfFile(mData);
        mData = nullptr;
    }
    if (mMappingHandle != nullptr)
    {
        CloseHandle(ToHandle(mMappingHandle));
        mMappingHandle = nullptr;
    }
#else
    if (mData != nullptr)
    {
        munmap(mData, static_cast<size_t>(mSize));
        mData = nullptr;
    }
#endif
}

void MemoryMappedFile::Flush(uint64_t offset, uint64_t size) const
{
    if (!mWritable || mData == nullptr || mSize == 0)
        return;

    CHECK_MSG(offset <= mSize, "Memory mapped flush offset is out of bounds");
    uint64_t flushSize = size == std::numeric_limits<uint64_t>::max() ? mSize - offset : size;
    CHECK_MSG(flushSize <= mSize - offset, "Memory mapped flush range is out of bounds");
    if (flushSize == 0)
        return;

    const unsigned char* flushBase = static_cast<const unsigned char*>(mData) + offset;
#if defined(_WIN32)
    CHECK_MSG(FlushViewOfFile(flushBase, CheckedSizeT(flushSize, "Memory mapped flush size")) != 0,
              "FlushViewOfFile failed: {}", LastSystemErrorString());
#else
    uint64_t pageSize = GetPageSize();
    uint64_t alignedOffset = offset - offset % pageSize;
    uint64_t alignmentDelta = offset - alignedOffset;
    uint64_t alignedFlushSize = flushSize + alignmentDelta;
    unsigned char* alignedBase = static_cast<unsigned char*>(mData) + alignedOffset;
    CHECK_MSG(msync(alignedBase, CheckedSizeT(alignedFlushSize, "Memory mapped flush size"), MS_SYNC) == 0,
              "msync failed: {}", LastSystemErrorString());
#endif
}

void MemoryMappedFile::Close() noexcept
{
    UnmapView();
#if defined(_WIN32)
    if (mFileHandle != nullptr)
    {
        CloseHandle(ToHandle(mFileHandle));
        mFileHandle = nullptr;
    }
#else
    if (mFd >= 0)
    {
        close(mFd);
        mFd = -1;
    }
#endif
    mSize = 0;
    mWritable = false;
}

bool MemoryMappedFile::IsOpen() const noexcept
{
#if defined(_WIN32)
    return mFileHandle != nullptr;
#else
    return mFd >= 0;
#endif
}

unsigned char* MemoryMappedFile::MutableData()
{
    CHECK_MSG(mWritable, "Memory mapped file is read-only");
    return static_cast<unsigned char*>(mData);
}

Span<const unsigned char> MemoryMappedFile::Bytes() const
{
    return { Data(), CheckedSizeT(mSize, "Memory mapped file size") };
}

Span<unsigned char> MemoryMappedFile::MutableBytes()
{
    return { MutableData(), CheckedSizeT(mSize, "Memory mapped file size") };
}

} // namespace Foundation::Core