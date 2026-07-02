#pragma once
#include <Core/Container.hpp>
#include <Core/MemoryMapped.hpp>
#include <Core/UUID.hpp>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <type_traits>
#if !defined(_WIN32)
#include <sys/types.h>
#endif
using namespace Foundation::Core;
constexpr uint32_t fourCC(const char a, const char b, const char c, const char d)
{
    return (a << 0) | (b << 8) | (c << 16) | (d << 24);
}
constexpr uint32_t fourCC(char const str[5])
{
    return fourCC(str[0], str[1], str[2], str[3]);
}
enum class FBlobCodec : uint32_t
{
    None = 0,
    LZ4 = 1,
};
struct FBlobRef
{
    uint64_t offset{0};
    uint64_t storedSize{0};
    uint64_t decodedSize{0};
    uint32_t count{0};
    uint32_t stride{0};
    FBlobCodec codec{FBlobCodec::None};
};
// RW primitives
struct FWriter
{
    virtual ~FWriter() = default;
    virtual size_t write(const void* data, size_t size) = 0;
    virtual bool seek(uint64_t offset) { return false; }
    virtual uint64_t tell() const { return 0; }
    virtual size_t operator()(const void* data, size_t size) { return write(data, size); }
};
struct FReader
{
    virtual ~FReader() = default;
    virtual size_t read(void* dest, size_t size) = 0;
    virtual bool seek(uint64_t offset) { return false; }
    virtual uint64_t tell() const { return 0; }
    virtual size_t operator()(void* dest, size_t size) { return read(dest, size); }
};

struct MemoryWriter : FWriter
{
    Vector<unsigned char>& data;
    uint64_t offset{0};

    explicit MemoryWriter(Vector<unsigned char>& data, uint64_t offset = 0);

    size_t write(const void* src, size_t size) override;
    bool seek(uint64_t offset) override;
    uint64_t tell() const override { return offset; }
};

struct MemoryReader : FReader
{
    Span<const unsigned char> data;
    uint64_t offset{0};

    explicit MemoryReader(Span<const unsigned char> data, uint64_t offset = 0);
    explicit MemoryReader(Vector<unsigned char> const& data, uint64_t offset = 0);

    size_t read(void* dest, size_t size) override;
    bool seek(uint64_t offset) override;
    uint64_t tell() const override { return offset; }
};

struct SpanWriter : FWriter
{
    Span<unsigned char> data;
    uint64_t offset{0};

    explicit SpanWriter(Span<unsigned char> data, uint64_t offset = 0);

    size_t write(const void* src, size_t size) override;
    bool seek(uint64_t offset) override;
    uint64_t tell() const override { return offset; }
};

uint64_t AlignUpU64(uint64_t value, uint64_t alignment);
void EnsureMappedFileSize(MemoryMappedFile& file, uint64_t requiredSize);

struct FBlobSerializer
{
    MemoryMappedFile& file;
    uint64_t& writeOffset;
    uint64_t baseOffset{0};
    Allocator* scratchAlloc{GLOBAL_ALLOC};

    explicit FBlobSerializer(MemoryMappedFile& file, uint64_t& writeOffset, uint64_t baseOffset = 0,
                             Allocator* scratchAlloc = GLOBAL_ALLOC);

    Span<unsigned char> Allocate(uint64_t size, uint64_t alignment, uint64_t& outPayloadOffset);

    FBlobRef AppendBytes(const void* data, size_t size, uint32_t count, uint32_t stride,
                         FBlobCodec codec = FBlobCodec::None, uint64_t alignment = 16);

    template <typename T>
    FBlobRef AppendArray(Vector<T> const& values, FBlobCodec codec = FBlobCodec::None)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        CHECK_MSG(values.size() <= UINT32_MAX, "FScene blob count exceeds uint32_t");
        return AppendBytes(values.data(), values.size() * sizeof(T),
                           static_cast<uint32_t>(values.size()), sizeof(T), codec);
    }
};

struct FBlobDeserializer
{
    Span<const unsigned char> payload;

    explicit FBlobDeserializer(Span<const unsigned char> payload);

    Span<const unsigned char> StoredBytes(FBlobRef const& blob) const;
    bool ReadBytes(FBlobRef const& blob, void* dst, size_t size, Allocator* scratchAlloc) const;

    template <typename T>
    bool ReadArray(FBlobRef const& blob, Vector<T>& values, Allocator* scratchAlloc) const
    {
        static_assert(std::is_trivially_copyable_v<T>);
        CHECK(blob.stride == sizeof(T));
        CHECK_MSG(blob.count <= SIZE_MAX / sizeof(T), "FScene blob count exceeds size_t");
        values.resize(static_cast<size_t>(blob.count));
        return ReadBytes(blob, values.data(), values.size() * sizeof(T), scratchAlloc);
    }

    template <typename T>
    Vector<T> ReadArray(FBlobRef const& blob, Allocator* alloc = GLOBAL_ALLOC) const
    {
        Vector<T> values(alloc);
        CHECK(ReadArray(blob, values, alloc));
        return values;
    }
};

// In-memory counterpart to FBlobSerializer. FBlobSerializer is the on-disk asset path
// (MemoryMappedFile-backed, optional LZ4); this one appends trivially-copyable arrays to a
// caller-owned byte vector and returns matching FBlobRefs, for code that serializes a
// resource straight into memory and hands it to a consumer that wants a payload + blobs
// (e.g. GPUScene::Upload in demos/tests). Blobs are stored verbatim (FBlobCodec::None), so
// the payload reads back without a scratch allocator.
struct MemoryBlobSerializer
{
    Vector<unsigned char>& payload;

    explicit MemoryBlobSerializer(Vector<unsigned char>& payload)
        : payload(payload)
    {
    }

    FBlobRef AppendBytes(const void* data, size_t size, uint32_t count, uint32_t stride, uint64_t alignment = 16);

    template <typename T>
    FBlobRef AppendArray(Vector<T> const& values, uint64_t alignment = 16)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        CHECK_MSG(values.size() <= UINT32_MAX, "Blob count exceeds uint32_t");
        return AppendBytes(values.data(), values.size() * sizeof(T),
                           static_cast<uint32_t>(values.size()), sizeof(T), alignment);
    }

    [[nodiscard]] FBlobDeserializer Deserializer() const
    {
        return FBlobDeserializer(Span<const unsigned char>(payload.data(), payload.size()));
    }
};

// Serialize
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
// Length-prefixed UTF-8 string.
inline void FSerialize(FWriter& writer, String const& str)
{
    uint64_t count = str.size();
    writer(&count, sizeof(uint64_t));
    if (count)
        writer(str.data(), static_cast<size_t>(count));
}
inline void FDeserialize(FReader& reader, String& str)
{
    uint64_t count = 0;
    reader(&count, sizeof(uint64_t));
    str.clear();
    str.resize(static_cast<size_t>(count));
    if (count)
        CHECK(reader.read(str.data(), static_cast<size_t>(count)) == static_cast<size_t>(count));
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
    CHECK_MSG(count <= SIZE_MAX, "Vector is too large for this platform");
    vec.clear();
    vec.reserve(static_cast<size_t>(count));
    if constexpr (std::is_trivially_copyable_v<T> && sizeof...(Args) == 0)
    {
        vec.resize(static_cast<size_t>(count));
        reader(vec.data(), static_cast<size_t>(count) * sizeof(T));
    }
    else
    {
        for (size_t i = 0; i < static_cast<size_t>(count); i++)
        {
            T& item = vec.emplace_back(args...);
            FDeserialize(reader, item);
        }
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