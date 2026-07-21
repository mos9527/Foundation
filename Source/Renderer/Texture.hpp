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
    [[nodiscard]] static uint64_t CalculateTextureImageSize(uint32_t width, uint32_t height, uint32_t depth,
                                                            uint32_t mipLevels, uint32_t blockSize,
                                                            uint32_t blockDim);
    [[nodiscard]] uint32_t GetSize() const;
    [[nodiscard]] size_t GetSubresourceSize(uint32_t layer, uint32_t mip) const;
    [[nodiscard]] RHIExtent3D GetMipExtent(uint32_t mipLevel) const;
    [[nodiscard]] RHITextureDesc GetDesc() const;
};

struct FSerializedTexture : FTextureHeader
{
    FUUID id{};
    FUUID name{};
    Vector<FBlobRef> subresources;

    explicit FSerializedTexture(Allocator* alloc = GLOBAL_ALLOC)
        : subresources(alloc)
    {
    }

    [[nodiscard]] uint32_t GetSubresourceCount() const { return GetNumLayers() * GetNumMips(); }

    [[nodiscard]] uint32_t GetSubresourceIndex(uint32_t layer, uint32_t mip) const
    {
        CHECK_MSG(layer < GetNumLayers(), "Texture layer {} out of range {}", layer, GetNumLayers());
        CHECK_MSG(mip < GetNumMips(), "Texture mip {} out of range {}", mip, GetNumMips());
        return layer * GetNumMips() + mip;
    }

    [[nodiscard]] FBlobRef const& GetSubresourceBlob(uint32_t layer, uint32_t mip) const
    {
        uint32_t const index = GetSubresourceIndex(layer, mip);
        CHECK_MSG(index < subresources.size(), "Texture subresource {} missing from serialized texture", index);
        return subresources[index];
    }

    [[nodiscard]] bool IsValid() const
    {
        return FTextureHeader::IsValid() && subresources.size() == GetSubresourceCount() && !subresources.empty();
    }
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
     * Generates full mipmap chain for uncompressed RGBA8 or RGBA32F textures.
     */
    void GenerateMips();
    /**
     * Encodes the current, uncompressed R8G8B8A8 texture into BC7 format
     */
    FTexture EncodeBC7(Allocator* alloc = GLOBAL_ALLOC) const;
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
void LoadHDR(FTexture& texture, Span<const unsigned char> data);
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

float4 SampleF32Bilinear(Span<const float4> mip, uint32_t mipW, uint32_t mipH, float u, float v);
float4 SampleF32Trilinear(const float4* const* mips, const uint32_t* mipW, const uint32_t* mipH, uint32_t mipCount,
                          float u, float v, float mip);
float2 EquirectDirectionToUV(float3 dir);
float3 EquirectUVToDirection(float2 uv);
/* -- Serialization -- */
template <>
inline void FSerialize(FWriter& w, FSerializedTexture const& obj)
{
    FSerialize(w, obj.id);
    FSerialize(w, obj.name);
    FTextureHeader const& header = obj;
    FSerialize(w, header.magic);
    FSerialize(w, header.header);
    FSerialize(w, header.header10);
    FSerialize(w, obj.subresources);
}

template <>
inline void FDeserialize(FReader& r, FSerializedTexture& obj)
{
    FDeserialize(r, obj.id);
    FDeserialize(r, obj.name);
    FTextureHeader& header = obj;
    FDeserialize(r, header.magic);
    FDeserialize(r, header.header);
    FDeserialize(r, header.header10);
    FDeserialize(r, obj.subresources);
}

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
