/* -- Generated with Gemini 3 Pro Preview */
#pragma once
#include <Core/Container.hpp>
#include <concepts>
#include <cstdint>
#include <cstring>
using namespace Foundation::Core;
struct FWriter
{
    Vector<char> buffer;
    FWriter(Allocator* alloc) : buffer(alloc) {}
    void operator()(const void* data, size_t size)
    {
        auto* cdata = static_cast<const char*>(data);
        buffer.insert(buffer.end(), cdata, cdata + size);
    }
};

struct FReader
{
    Span<const char> buffer;
    void operator()(void* dest, size_t size)
    {
        CHECK(buffer.size_bytes() >= size);
        std::memcpy(dest, buffer.data(), size);
        buffer = buffer.subspan(size);
    }
};
template <typename T>
void FSerialize(FWriter& w, const T& obj);
template <typename T>
void FDeserialize(FReader& r, T& obj);
// -----------------------------------------------------------------------------
// 2. Generic SFINAE Templates
// -----------------------------------------------------------------------------
// Generic Serialize: Handles PODs automatically
template <typename T>
void FSerialize(FWriter& writer, const T& obj)
    requires std::is_trivially_copyable_v<T>
{
    writer(&obj, sizeof(T));
}

// Generic Deserialize: Handles PODs automatically
template <typename T>
void FDeserialize(FReader& reader, T& obj)
    requires std::is_trivially_copyable_v<T>
{
    reader(&obj, sizeof(T));
}

template <typename T>
void FSerialize(FWriter& writer, const Vector<T>& vec)
{
    uint64_t count = vec.size();
    writer(&count, sizeof(uint64_t));
    if constexpr (std::is_trivially_copyable_v<T>)
        writer(vec.data(), count * sizeof(T));
    else
    {
        for (const auto& item : vec)
            FSerialize(writer, item);
    }
}

template <typename T, typename... Args>
void FDeserialize(FReader& reader, Vector<T>& vec, Args const&... args)
{
    uint64_t count = 0;
    reader(&count, sizeof(uint64_t));
    vec.resize(count, args...);
    if constexpr (std::is_trivially_copyable_v<T>)
        reader(vec.data(), count * sizeof(T));
    else
    {
        for (size_t i = 0; i < count; i++)
            FDeserialize(reader, vec[i]);
    }
}
