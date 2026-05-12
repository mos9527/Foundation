#include "Serialization.hpp"

#include <algorithm>
#include <climits>
#include <lz4.h>

uint64_t AlignUpU64(uint64_t value, uint64_t alignment)
{
    CHECK(alignment != 0);
    return (value + alignment - 1) / alignment * alignment;
}

void WriteZeroBytes(FWriter& writer, uint64_t bytes)
{
    static constexpr unsigned char kZeroes[4096]{};
    while (bytes != 0)
    {
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(bytes, sizeof(kZeroes)));
        CHECK(writer.write(kZeroes, chunk) == chunk);
        bytes -= chunk;
    }
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

FBlobSerializer::FBlobSerializer(FWriter& writer, uint64_t baseOffset)
    : writer(writer), baseOffset(baseOffset)
{
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
    Vector<char> compressed(GLOBAL_ALLOC);
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
    CHECK_MSG(writer.tell() >= baseOffset, "Blob writer is before its payload base offset");
    uint64_t currentPayloadOffset = writer.tell() - baseOffset;
    uint64_t aligned = AlignUpU64(currentPayloadOffset, alignment);
    if (aligned > currentPayloadOffset)
        WriteZeroBytes(writer, aligned - currentPayloadOffset);

    uint64_t fileOffset = writer.tell();
    CHECK_MSG(fileOffset >= baseOffset, "Blob write before payload base offset");
    ref.offset = fileOffset - baseOffset;
    CHECK(writer.write(writeData, writeSize) == writeSize);
    return ref;
}

FBlobDeserializer::FBlobDeserializer(FReader& reader, uint64_t baseOffset)
    : reader(reader), baseOffset(baseOffset)
{
}

bool FBlobDeserializer::ReadBytes(FBlobRef const& blob, void* dst, size_t size) const
{
    CHECK(blob.decodedSize == size);
    if (size == 0)
        return true;

    CHECK(dst != nullptr);
    CHECK(reader.seek(baseOffset + blob.offset));
    switch (blob.codec)
    {
    case FBlobCodec::None:
        CHECK(blob.storedSize == size);
        return reader.read(dst, size) == size;
    case FBlobCodec::LZ4:
    {
        CHECK_MSG(blob.storedSize <= static_cast<uint64_t>(INT_MAX), "LZ4 blob too large: {} bytes", blob.storedSize);
        CHECK_MSG(size <= static_cast<size_t>(INT_MAX), "LZ4 decoded blob too large: {} bytes", size);
        Vector<char> compressed(static_cast<size_t>(blob.storedSize), GLOBAL_ALLOC);
        if (reader.read(compressed.data(), compressed.size()) != compressed.size())
            return false;
        int decodedSize = LZ4_decompress_safe(compressed.data(), static_cast<char*>(dst),
                                              static_cast<int>(compressed.size()), static_cast<int>(size));
        return decodedSize == static_cast<int>(size);
    }
    default:
        CHECK_MSG(false, "Unsupported blob codec {}", static_cast<uint32_t>(blob.codec));
        return false;
    }
}

bool FBlobDeserializer::ReadBytesRange(FBlobRef const& blob, uint64_t srcOffset, void* dst, size_t size) const
{
    CHECK_MSG(blob.codec == FBlobCodec::None, "Blob range reads require uncompressed blobs");
    CHECK_MSG(srcOffset <= blob.decodedSize, "Blob range offset {} exceeds decoded size {}", srcOffset, blob.decodedSize);
    CHECK_MSG(static_cast<uint64_t>(size) <= blob.decodedSize - srcOffset,
              "Blob range size {} at offset {} exceeds decoded size {}", size, srcOffset, blob.decodedSize);
    CHECK_MSG(blob.storedSize == blob.decodedSize, "Uncompressed blob stored size mismatch");
    if (size == 0)
        return true;

    CHECK(dst != nullptr);
    CHECK_MSG(srcOffset <= UINT64_MAX - blob.offset, "Blob range file offset overflows");
    uint64_t blobFileOffset = blob.offset + srcOffset;
    CHECK_MSG(baseOffset <= UINT64_MAX - blobFileOffset, "Blob range absolute offset overflows");
    CHECK(reader.seek(baseOffset + blobFileOffset));
    return reader.read(dst, size) == size;
}

