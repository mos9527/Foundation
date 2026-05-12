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

struct FTextureHeader
{
    uint32_t magic{DDS_MAGIC};
    DDS_HEADER header{};
    DDS_HEADER_DXT10 header10{};

    [[nodiscard]] bool IsValid() const { return magic == DDS_MAGIC && GetSize() != 0; }
    [[nodiscard]] uint32_t GetWidth() const { return header.width; }
    [[nodiscard]] uint32_t GetHeight() const { return header.height; }
    [[nodiscard]] uint32_t GetDepth() const;
    [[nodiscard]] RHITextureDimension GetDimension() const;
    [[nodiscard]] RHITextureDimension GetViewDimension() const;
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
    [[nodiscard]] uint32_t GetBlockSize() const;
    [[nodiscard]] uint32_t GetBpp() const;
    [[nodiscard]] uint32_t GetSize() const;
    [[nodiscard]] RHIExtent3D GetMipExtent(uint32_t mipLevel) const;
    [[nodiscard]] RHITextureDesc GetDesc() const;
};

struct FSerializedTexture : FTextureHeader
{
    FBlobRef data;

    [[nodiscard]] bool IsValid() const { return FTextureHeader::IsValid() && data.storedSize != 0; }
};

struct FTexture : FTextureHeader
{
    mutable Vector<unsigned char> bytes;

    FTexture(Allocator* alloc);
    void Initialize(RHIResourceFormat format, RHITextureDimension dimension, uint32_t width, uint32_t height = 1,
                    uint32_t depth = 1, uint32_t mipCount = 1, uint32_t layerCount = 1);
    [[nodiscard]] bool IsValid() const { return FTextureHeader::IsValid() && bytes.size() == GetSize(); }
    // Get raw subresource slice that can be uploaded to the GPU directly
    [[nodiscard]] Span<unsigned char> GetSubresource(uint32_t mipLevel = 0, uint32_t arrayLayer = 0) const;

    /**
     * Generates full mipmap chain for an uncompressed R8G8B8A8 texture
     */
    void GenerateMips();
    /**
     * Encodes the current, uncompressed R8G8B8A8 texture into BC7 format
     */
    FTexture EncodeBC7() const;

    [[nodiscard]] FSerializedTexture ToSerializedTexture(FBlobRef blob = {}) const
    {
        FSerializedTexture texture;
        texture.magic = magic;
        texture.header = header;
        texture.header10 = header10;
        texture.data = blob;
        return texture;
    }
};

/**
 * Loads a DDS file into a texture
 * @note This is an alias of @ref FSerialize. @ref FTexture is in fact in standard DDS format.
 */
void LoadDDS(FTexture& texture, StringView path);
/**
 * Loads standard image formats (PNG, JPG, etc.) via stb_image
 * into an uncompressed R8G8B8A8 texture
 * @param gamma Whether to load the image as sRGB (true) or linear (false) encoding.
 */
void LoadRGBA8(FTexture& texture, StringView path, bool gamma = true);
/**
 * Loads standard image formats (PNG, JPG, etc.) via stb_image
 * into an uncompressed R8G8B8A8 texture from memory
 * @param gamma Whether to load the image as sRGB (true) or linear (false) encoding.
 */
void LoadRGBA8(FTexture& texture, Span<const unsigned char> data, bool gamma = true);
/**
 * Loads HDR image formats (.hdr) via stb_image into an R32G32B32A32Float texture.
 * Used for environment maps.
 */
void LoadHDR(FTexture& texture, StringView path);
/**
 * Saves float RGBA pixel data as an HDR image.
 * Output format is Radiance HDR (stb_image_write), RGB only (alpha discarded).
 * @param data Pointer to float RGBA pixel data (4 floats per pixel).
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param path Output file path.
 */
void SaveHDR(const float* data, int width, int height, StringView path);
/**
 * Saves RGBA8 data as a PNG (.png) file via stb_image_write.
 * @param data Pointer to uint8 RGBA pixel data (4 bytes per pixel).
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @param path Output file path.
 */
void SavePNG(const unsigned char* data, int width, int height, StringView path);
/* -- Serialization -- */
template <>
inline void FSerialize(FWriter& w, FTexture const& obj)
{
    FSerialize(w, obj.magic);
    FSerialize(w, obj.header);
    if (obj.header.ddspf.fourCC == DDSPF_DX10.fourCC)
        FSerialize(w, obj.header10);
    CHECK(w.write(obj.bytes.data(), obj.bytes.size()) == obj.bytes.size());
}
template <>
inline void FDeserialize(FReader& r, FTexture& obj)
{
    FDeserialize(r, obj.magic);
    CHECK(obj.magic == DDS_MAGIC);
    FDeserialize(r, obj.header);
    if (obj.header.ddspf.fourCC == DDSPF_DX10.fourCC)
        FDeserialize(r, obj.header10);
    obj.bytes.resize(obj.GetSize());
    CHECK(r.read(obj.bytes.data(), obj.bytes.size()) == obj.bytes.size());
}
