#pragma once
#include <Core/Container.hpp>
#include <concepts>
#include <cstdint>
#include <cstring>
using namespace Foundation::Core;
// RW primitives
struct FWriter
{
    virtual ~FWriter() = default;
    virtual void operator()(const void* data, size_t size) = 0;
};
struct FReader
{
    virtual ~FReader() = default;
    virtual void operator()(void* dest, size_t size) = 0;
};
template <typename T>
void FSerialize(FWriter& w, const T& obj);
template <typename T>
void FDeserialize(FReader& r, T& obj);
// POD Data
template <typename T>
void FSerialize(FWriter& writer, const T& obj)
    requires std::is_trivially_copyable_v<T>
{
    writer(&obj, sizeof(T));
}
template <typename T>
void FDeserialize(FReader& reader, T& obj)
    requires std::is_trivially_copyable_v<T>
{
    reader(&obj, sizeof(T));
}
// Custom serialization
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
// File IO
struct FileWriter : FWriter
{
    FILE* fp;
    FileWriter(StringView path)
    {
        fp = fopen(path.data(), "wb");
        CHECK_MSG(fp != nullptr, "Can't open {}", path);
    }
    ~FileWriter() override { fflush(fp), fclose(fp); }
    void operator()(const void* data, size_t size) override { fwrite(data, 1, size, fp); }
};
struct FileReader : FReader
{
    FILE* fp;
    FileReader(StringView path)
    {
        fp = fopen(path.data(), "rb");
        CHECK_MSG(fp != nullptr, "Can't open {}", path);
    }
    void operator()(void* dest, size_t size) override { fread(dest, 1, size, fp); }
};
