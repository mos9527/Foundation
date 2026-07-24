#include "Mmap.hpp"
#include <Core/Logging.hpp>

#include <cerrno>
#include <cstring>
#include <limits>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace Foundation::Platform {
namespace {

size_t CheckedSizeT(uint64_t value, const char* name)
{
    CHECK_MSG(value <= static_cast<uint64_t>(std::numeric_limits<size_t>::max()),
              "{} exceeds size_t range", name);
    return static_cast<size_t>(value);
}

HANDLE ToHandle(std::intptr_t handle)
{
    return reinterpret_cast<HANDLE>(handle);
}

std::intptr_t FromHandle(HANDLE handle)
{
    return reinterpret_cast<std::intptr_t>(handle);
}

Core::String LastSystemErrorString()
{
    DWORD error = GetLastError();
    if (error == 0)
        return {};

    LPSTR message = nullptr;
    DWORD length = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                      FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                  reinterpret_cast<LPSTR>(&message), 0, nullptr);
    Core::String result;
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

void UnmapView(MmapState& state) noexcept
{
    if (state.data != nullptr)
    {
        UnmapViewOfFile(state.data);
        state.data = nullptr;
    }
    if (state.mapping != nullptr)
    {
        CloseHandle(static_cast<HANDLE>(state.mapping));
        state.mapping = nullptr;
    }
}

void MapView(MmapState& state)
{
    if (state.size == 0)
        return;

    DWORD protect = state.writable ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE mapping = CreateFileMappingA(ToHandle(state.file), nullptr, protect, 0, 0, nullptr);
    CHECK_MSG(mapping != nullptr, "CreateFileMappingA failed: {}", LastSystemErrorString());
    state.mapping = mapping;

    DWORD desiredAccess = state.writable ? FILE_MAP_WRITE : FILE_MAP_READ;
    state.data = MapViewOfFile(mapping, desiredAccess, 0, 0, 0);
    CHECK_MSG(state.data != nullptr, "MapViewOfFile failed: {}", LastSystemErrorString());
}

} // namespace

void MmapOpen(MmapState& state, Core::StringView path, MmapAccess access)
{
    MmapClose(state);
    state.writable = access == MmapAccess::ReadWrite;
    Core::String pathString(path);

    DWORD desiredAccess = state.writable ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    DWORD shareMode = FILE_SHARE_READ | (state.writable ? 0 : FILE_SHARE_WRITE);
    HANDLE file = CreateFileA(pathString.c_str(), desiredAccess, shareMode, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK_MSG(file != INVALID_HANDLE_VALUE, "CreateFileA failed for {}: {}", pathString, LastSystemErrorString());
    state.file = FromHandle(file);
    state.size = GetFileSize64(file);
    MapView(state);
}

void MmapCreate(MmapState& state, Core::StringView path, uint64_t size)
{
    MmapClose(state);
    state.writable = true;
    state.size = size;
    Core::String pathString(path);

    HANDLE file = CreateFileA(pathString.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK_MSG(file != INVALID_HANDLE_VALUE, "CreateFileA failed for {}: {}", pathString, LastSystemErrorString());
    state.file = FromHandle(file);
    SetFileSize64(file, size);
    MapView(state);
}

void MmapOpenOrCreate(MmapState& state, Core::StringView path, uint64_t size)
{
    MmapClose(state);
    state.writable = true;
    Core::String pathString(path);

    HANDLE file = CreateFileA(pathString.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK_MSG(file != INVALID_HANDLE_VALUE, "CreateFileA failed for {}: {}", pathString, LastSystemErrorString());
    state.file = FromHandle(file);
    uint64_t currentSize = GetFileSize64(file);
    if (currentSize != size)
        SetFileSize64(file, size);
    state.size = size;
    MapView(state);
}

void MmapResize(MmapState& state, uint64_t size)
{
    CHECK_MSG(MmapIsOpen(state), "Cannot resize a closed memory mapped file");
    CHECK_MSG(state.writable, "Cannot resize a read-only memory mapped file");
    if (state.size == size)
        return;

    MmapFlush(state, 0, std::numeric_limits<uint64_t>::max());
    UnmapView(state);
    SetFileSize64(ToHandle(state.file), size);
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

    const unsigned char* flushBase = static_cast<const unsigned char*>(state.data) + offset;
    CHECK_MSG(FlushViewOfFile(flushBase, CheckedSizeT(flushSize, "Memory mapped flush size")) != 0,
              "FlushViewOfFile failed: {}", LastSystemErrorString());
}

void MmapClose(MmapState& state) noexcept
{
    UnmapView(state);
    if (state.file != -1)
    {
        CloseHandle(ToHandle(state.file));
        state.file = -1;
    }
    state.size = 0;
    state.writable = false;
}

bool MmapIsOpen(MmapState const& state) noexcept
{
    return state.file != -1;
}

} // namespace Foundation::Platform
