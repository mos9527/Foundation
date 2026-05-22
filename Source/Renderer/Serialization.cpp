#include "Serialization.hpp"

#include <algorithm>
#include <climits>
#include <lz4.h>

uint64_t AlignUpU64(uint64_t value, uint64_t alignment)
{
    CHECK(alignment != 0);
    return (value + alignment - 1) / alignment * alignment;
}

MemoryWriter::MemoryWriter(Vector<unsigned char>& data, uint64_t offset)
    : data(data), offset(offset)
{
    CHECK_MSG(offset <= SIZE_MAX, "MemoryWriter offset too large for this platform");
}

size_t MemoryWriter::write(const void* src, size_t size)
{
    if (size == 0)
        return 0;

    CHECK(src != nullptr);
    CHECK_MSG(offset <= SIZE_MAX, "MemoryWriter offset too large for this platform");
    size_t begin = static_cast<size_t>(offset);
    CHECK_MSG(size <= SIZE_MAX - begin, "MemoryWriter write range overflows");
    size_t end = begin + size;
    if (end > data.size())
        data.resize(end, 0u);
    std::memcpy(data.data() + begin, src, size);
    offset = end;
    return size;
}

bool MemoryWriter::seek(uint64_t newOffset)
{
    CHECK_MSG(newOffset <= SIZE_MAX, "MemoryWriter offset too large for this platform");
    offset = newOffset;
    return true;
}

MemoryReader::MemoryReader(Span<const unsigned char> data, uint64_t offset)
    : data(data), offset(offset)
{
    CHECK_MSG(offset <= data.size(), "MemoryReader offset is out of bounds");
}

MemoryReader::MemoryReader(Vector<unsigned char> const& data, uint64_t offset)
    : MemoryReader(Span<const unsigned char>(data.data(), data.size()), offset)
{
}

size_t MemoryReader::read(void* dest, size_t size)
{
    if (size == 0)
        return 0;
    CHECK(dest != nullptr);
    if (offset >= data.size())
        return 0;

    size_t begin = static_cast<size_t>(offset);
    size_t readSize = std::min(size, data.size() - begin);
    std::memcpy(dest, data.data() + begin, readSize);
    offset += readSize;
    return readSize;
}

bool MemoryReader::seek(uint64_t newOffset)
{
    if (newOffset > data.size())
        return false;
    offset = newOffset;
    return true;
}

void EnsureMappedFileSize(MemoryMappedFile& file, uint64_t requiredSize)
{
    CHECK(file.IsWritable());
    if (requiredSize <= file.Size())
        return;

    uint64_t newSize = file.Size() != 0 ? file.Size() : 1;
    while (newSize < requiredSize)
    {
        CHECK_MSG(newSize <= UINT64_MAX / 2, "Mapped file size overflow while growing to {} bytes", requiredSize);
        newSize *= 2;
    }
    file.Resize(newSize);
}

SpanWriter::SpanWriter(Span<unsigned char> data, uint64_t offset)
    : data(data), offset(offset)
{
    CHECK_MSG(offset <= data.size(), "SpanWriter offset is out of bounds");
}

size_t SpanWriter::write(const void* src, size_t size)
{
    if (size == 0)
        return 0;

    CHECK(src != nullptr);
    CHECK_MSG(offset <= data.size(), "SpanWriter offset is out of bounds");
    size_t begin = static_cast<size_t>(offset);
    size_t writeSize = std::min(size, data.size() - begin);
    std::memcpy(data.data() + begin, src, writeSize);
    offset += writeSize;
    return writeSize;
}

bool SpanWriter::seek(uint64_t newOffset)
{
    if (newOffset > data.size())
        return false;
    offset = newOffset;
    return true;
}

FBlobSerializer::FBlobSerializer(MemoryMappedFile& file, uint64_t& writeOffset, uint64_t baseOffset,
                                   Allocator* scratchAlloc)
    : file(file), writeOffset(writeOffset), baseOffset(baseOffset), scratchAlloc(scratchAlloc)
{
    CHECK(file.IsWritable());
    CHECK(scratchAlloc != nullptr);
    CHECK_MSG(writeOffset >= baseOffset, "Blob writer is before its payload base offset");
}

Span<unsigned char> FBlobSerializer::Allocate(uint64_t size, uint64_t alignment, uint64_t& outPayloadOffset)
{
    CHECK(alignment != 0);
    CHECK_MSG(writeOffset >= baseOffset, "Blob writer is before its payload base offset");
    uint64_t currentPayloadOffset = writeOffset - baseOffset;
    uint64_t alignedPayloadOffset = AlignUpU64(currentPayloadOffset, alignment);
    CHECK_MSG(baseOffset <= UINT64_MAX - alignedPayloadOffset, "Blob absolute offset overflows");
    uint64_t absoluteOffset = baseOffset + alignedPayloadOffset;
    CHECK_MSG(size <= UINT64_MAX - absoluteOffset, "Blob write range overflows");
    uint64_t endOffset = absoluteOffset + size;

    EnsureMappedFileSize(file, endOffset);
    if (absoluteOffset > writeOffset)
        std::memset(file.MutableData() + writeOffset, 0, static_cast<size_t>(absoluteOffset - writeOffset));

    outPayloadOffset = alignedPayloadOffset;
    writeOffset = endOffset;
    return {file.MutableData() + absoluteOffset, static_cast<size_t>(size)};
}

FBlobRef FBlobSerializer::AppendBytes(const void* data, size_t size, uint32_t count, uint32_t stride,
                                      FBlobCodec codec, uint64_t alignment)
{
    FBlobRef ref{};
    ref.decodedSize = uint64_t(count) * stride;
    ref.storedSize = size;
    ref.count = count;
    ref.stride = stride;
    ref.codec = FBlobCodec::None;
    CHECK_MSG(ref.decodedSize == size, "Blob size mismatch: {} bytes for {} elements with stride {}", size, count, stride);
    if (size == 0)
        return ref;

    CHECK(data != nullptr);

    const void* writeData = data;
    size_t writeSize = size;
    Vector<char> compressed(scratchAlloc);
    if (codec == FBlobCodec::LZ4)
    {
        CHECK_MSG(size <= static_cast<size_t>(INT_MAX), "LZ4 blob too large: {} bytes", size);
        int maxCompressedSize = LZ4_compressBound(static_cast<int>(size));
        CHECK_MSG(maxCompressedSize > 0, "LZ4 compression bound failed for {} bytes", size);
        compressed.resize(static_cast<size_t>(maxCompressedSize));
        int compressedSize = LZ4_compress_default(static_cast<const char*>(data), compressed.data(),
                                                  static_cast<int>(size), maxCompressedSize);
        CHECK_MSG(compressedSize > 0, "LZ4 compression failed for {} bytes", size);
        if (static_cast<size_t>(compressedSize) < size)
        {
            writeData = compressed.data();
            writeSize = static_cast<size_t>(compressedSize);
            ref.codec = FBlobCodec::LZ4;
        }
    }

    ref.storedSize = writeSize;
    uint64_t payloadOffset = 0;
    Span<unsigned char> dst = Allocate(writeSize, alignment, payloadOffset);
    ref.offset = payloadOffset;
    std::memcpy(dst.data(), writeData, writeSize);
    return ref;
}

FBlobDeserializer::FBlobDeserializer(Span<const unsigned char> payload)
    : payload(payload)
{
}

Span<const unsigned char> FBlobDeserializer::StoredBytes(FBlobRef const& blob) const
{
    CHECK_MSG(blob.offset <= payload.size(), "Blob offset exceeds payload size");
    CHECK_MSG(blob.storedSize <= payload.size() - blob.offset, "Blob exceeds payload size");
    return {payload.data() + blob.offset, static_cast<size_t>(blob.storedSize)};
}

bool FBlobDeserializer::ReadBytes(FBlobRef const& blob, void* dst, size_t size, Allocator* scratchAlloc) const
{
    CHECK(blob.decodedSize == size);
    if (size == 0)
        return true;

    CHECK(dst != nullptr);
    Span<const unsigned char> stored = StoredBytes(blob);
    switch (blob.codec)
    {
    case FBlobCodec::None:
        CHECK(blob.storedSize == size);
        std::memcpy(dst, stored.data(), size);
        return true;
    case FBlobCodec::LZ4:
    {
        CHECK(scratchAlloc != nullptr);
        CHECK_MSG(blob.storedSize <= static_cast<uint64_t>(INT_MAX), "LZ4 blob too large: {} bytes", blob.storedSize);
        CHECK_MSG(size <= static_cast<size_t>(INT_MAX), "LZ4 decoded blob too large: {} bytes", size);
        // We'd like to decompress on the CPU since otherwise we'd assume that dst is RW
        // With ReBAR devices this _is_ true - without cache. Avoid it for now.
        // TODO: GDeflate?
        Vector<char> decoded(scratchAlloc);
        decoded.resize(size);
        int decodedSize = LZ4_decompress_safe(reinterpret_cast<const char*>(stored.data()), decoded.data(),
                                              static_cast<int>(stored.size()), static_cast<int>(size));
        if (decodedSize != static_cast<int>(size))
            return false;
        std::memcpy(dst, decoded.data(), size);
        return true;
    }
    default:
        CHECK_MSG(false, "Unsupported blob codec {}", static_cast<uint32_t>(blob.codec));
        return false;
    }
}
