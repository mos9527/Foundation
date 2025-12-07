#pragma once
#include <Core/Container.hpp>
#include <Math/Math.hpp>
#include "Serialization.hpp"
#include "TextureDDS.hpp"
using namespace Foundation;
using namespace RHI;
using namespace Core;
using namespace Math;

struct FTexture2D
{
    uint32_t magic;
    DDS_HEADER header{};
    DDS_HEADER_DXT10 header10{};
    mutable Vector<unsigned char> data;

    FTexture2D(Allocator* alloc) : magic(DDS_MAGIC), data(alloc) {}
    [[nodiscard]] uint32_t GetWidth() const { return header.width; }
    [[nodiscard]] uint32_t GetHeight() const { return header.height; }
    [[nodiscard]] uint32_t GetNumLayers() const
    {
        if (header.caps2 & DDS_CUBEMAP)
            return 6;
        if (header10.arraySize)
            return header10.arraySize;
        return 1;
    }
    [[nodiscard]] uint32_t GetNumMips() const { return header.mipMapCount > 0 ? header.mipMapCount : 1; }
    [[nodiscard]] RHIResourceFormat GetFormat() const;
    // Bits per BC block. NOTE: 0 for non-block-compressed formats
    [[nodiscard]] uint32_t GetBlockSize() const;
    // Bits per pixel. NOTE: 0 for block-compressed formats
    [[nodiscard]] uint32_t GetBpp() const;
    [[nodiscard]] uint32_t GetSize() const;
    [[nodiscard]] Span<unsigned char> GetSubresource(uint32_t mipLevel = 0, uint32_t arrayLayer = 0) const;

    /**
     * Generates full mipmap chain for an uncompressed R8G8B8A8 texture
     */
    void GenerateMips();
    /**
     * Encodes the current, uncompressed R8G8B8A8 texture into BC7 format
     */
    void EncodeBC7(FTexture2D& outTexture) const;
};

/**
 * Loads a DDS file into a texture
 * @note This is an alias of @ref FSerialize. @ref FTexture2D is in fact in standard DDS format.
 */
void LoadDDS(FTexture2D& texture, StringView path);
/**
 * Loads standard image formats (PNG, JPG, etc.) via stb_image
 * into an uncompressed R8G8B8A8 texture
 * @param gamma Whether to load the image as sRGB (true) or linear (false) encoding.
 */
void LoadRGBA8(FTexture2D& texture, StringView path, bool gamma = true);

/* -- Serialization -- */
template <>
inline void FSerialize(FWriter& w, FTexture2D const& obj)
{
    FSerialize(w, obj.magic);
    FSerialize(w, obj.header);
    if (obj.header.ddspf.fourCC == DDSPF_DX10.fourCC)
        FSerialize(w, obj.header10);
    CHECK(w.write(obj.data.data(), obj.data.size()) == obj.data.size());
}
template <>
inline void FDeserialize(FReader& r, FTexture2D& obj)
{
    FDeserialize(r, obj.magic);
    CHECK(obj.magic == DDS_MAGIC);
    FDeserialize(r, obj.header);
    if (obj.header.ddspf.fourCC == DDSPF_DX10.fourCC)
        FDeserialize(r, obj.header10);
    obj.data.resize(obj.GetSize());
    CHECK(r.read(obj.data.data(), obj.data.size()) == obj.data.size());
}
