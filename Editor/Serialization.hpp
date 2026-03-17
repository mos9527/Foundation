#pragma once
#include <Core/Container.hpp>
#include <concepts>
#include <cstdint>
#include <cstring>
using namespace Foundation::Core;
constexpr uint32_t fourCC(const char a, const char b, const char c, const char d)
{
    return (a << 0) | (b << 8) | (c << 16) | (d << 24);
}
constexpr uint32_t fourCC(char const str[5])
{
    return fourCC(str[0], str[1], str[2], str[3]);
}
// RW primitives
struct FWriter
{
    virtual ~FWriter() = default;
    virtual size_t write(const void* data, size_t size) = 0;
    virtual size_t operator()(const void* data, size_t size) { return write(data, size); }
};
struct FReader
{
    virtual ~FReader() = default;
    virtual size_t read(void* dest, size_t size) = 0;
    virtual size_t operator()(void* dest, size_t size) { return read(dest, size); }
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
// Optional<T>
template <typename T>
void FSerialize(FWriter& writer, const Optional<T>& opt)
{
    uint8_t hasValue = opt.has_value() ? 1 : 0;
    FSerialize(writer, hasValue);
    if (hasValue)
        FSerialize(writer, *opt);
}
template <typename T, typename... Args>
void FDeserialize(FReader& reader, Optional<T>& opt, Args const&... args)
{
    uint8_t hasValue = 0;
    FDeserialize(reader, hasValue);
    if (hasValue)
    {
        opt.emplace(args...);
        FDeserialize(reader, *opt);
    }
    else
    {
        opt = std::nullopt;
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
    size_t write(const void* data, size_t size) override { return fwrite(data, 1, size, fp); }
};
struct FileReader : FReader
{
    FILE* fp;
    FileReader(StringView path)
    {
        fp = fopen(path.data(), "rb");
        CHECK_MSG(fp != nullptr, "Can't open {}", path);
    }
    size_t read(void* dest, size_t size) override { return fread(dest, 1, size, fp); }
};
