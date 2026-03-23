#pragma once
#include <Core/Container.hpp>
#include <Math/Math.hpp>
#include <RHICore/Resource.hpp>
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

    FTexture2D(Allocator* alloc);
    [[nodiscard]] bool IsValid() const { return magic == DDS_MAGIC && GetSize(); }
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
    // BYTES per BC block. NOTE: 0 for non-block-compressed formats
    [[nodiscard]] uint32_t GetBlockSize() const;
    // Bits per pixel. NOTE: 0 for block-compressed formats
    [[nodiscard]] uint32_t GetBpp() const;
    [[nodiscard]] uint32_t GetSize() const;

    [[nodiscard]] RHITextureDesc GetDesc() const;
    // Get raw subresource slice that can be uploaded to the GPU directly
    [[nodiscard]] Span<unsigned char> GetSubresource(uint32_t mipLevel = 0, uint32_t arrayLayer = 0) const;

    /**
     * Generates full mipmap chain for an uncompressed R8G8B8A8 texture
     */
    void GenerateMips();
    /**
     * Encodes the current, uncompressed R8G8B8A8 texture into BC7 format
     */
    FTexture2D EncodeBC7() const;
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
/**
 * Loads standard image formats (PNG, JPG, etc.) via stb_image
 * into an uncompressed R8G8B8A8 texture from memory
 * @param gamma Whether to load the image as sRGB (true) or linear (false) encoding.
 */
void LoadRGBA8(FTexture2D& texture, Span<const unsigned char> data, bool gamma = true);
/**
 * Loads HDR image formats (.hdr) via stb_image into an R32G32B32A32Float texture.
 * Used for environment maps.
 */
void LoadHDR(FTexture2D& texture, StringView path);
/**
 * Saves float data as an HDR (.hdr) file via stb_image_write.
 * @param data Pointer to float RGBA pixel data (4 floats per pixel).
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param path Output file path.
 */
void SaveHDR(const float* data, int width, int height, StringView path);
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
