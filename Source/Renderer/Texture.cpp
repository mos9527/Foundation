#include "Texture.hpp"
#include <bc7enc.h>
#include <limits>
#include <stb_image_write.h>
using namespace Foundation::RHI;
FTexture::FTexture(Allocator* alloc) : bytes(alloc)
{
    magic = DDS_MAGIC;
    static bool bc7encInitialized = false;
    if (!bc7encInitialized)
        bc7enc_compress_block_init(), bc7encInitialized = true;
}
void FTexture::Initialize(RHIResourceFormat format, RHITextureDimension dimension, uint32_t width, uint32_t height,
                          uint32_t depth, uint32_t mipCount, uint32_t layerCount)
{
    CHECK_MSG(width && height && depth && mipCount && layerCount,
              "Texture dimensions, mip count, and layer count must be non-zero");
    CHECK_MSG(dimension != RHITextureDimension::E1D || (height == 1 && depth == 1),
              "1D textures must have height/depth of 1");
    CHECK_MSG(dimension != RHITextureDimension::E2D || depth == 1, "2D textures must have depth of 1");
    CHECK_MSG(dimension != RHITextureDimension::E3D || layerCount == 1, "3D texture arrays are not supported");

    ddsCreateHeader(header, width, height, mipCount, depth);
    ddsSetFormat(header, header10, layerCount, format, dimension);
    bytes.clear();
}
RHITextureDimension FTextureHeader::GetDimension() const
{
    if (header.ddspf.fourCC == DDSPF_DX10.fourCC)
    {
        switch (header10.resourceDimension)
        {
        case DDS_RESOURCE_DIMENSION::DDS_DIMENSION_TEXTURE1D:
            return RHITextureDimension::E1D;
        case DDS_RESOURCE_DIMENSION::DDS_DIMENSION_TEXTURE3D:
            return RHITextureDimension::E3D;
        default:
            return RHITextureDimension::E2D;
        }
    }
    if ((header.flags & DDS_HEADER_FLAGS_VOLUME) || (header.caps2 & DDS_FLAGS_VOLUME))
        return RHITextureDimension::E3D;
    return RHITextureDimension::E2D;
}
RHITextureDimension FTextureHeader::GetViewDimension() const
{
    if (header.caps2 & DDS_CUBEMAP)
        return GetNumLayers() > 6 ? RHITextureDimension::ECubeArray : RHITextureDimension::ECube;
    RHITextureDimension dimension = GetDimension();
    if (GetNumLayers() > 1)
    {
        if (dimension == RHITextureDimension::E1D)
            return RHITextureDimension::E1DArray;
        if (dimension == RHITextureDimension::E2D)
            return RHITextureDimension::E2DArray;
    }
    return dimension;
}
uint32_t FTextureHeader::GetDepth() const
{
    return GetDimension() == RHITextureDimension::E3D ? std::max(1u, header.depth) : 1u;
}
RHIResourceFormat FTextureHeader::GetFormat() const
{
    using enum RHIResourceFormat;
    switch (header.ddspf.fourCC)
    {
    case DDSPF_DXT1.fourCC:
        return Bc1RgbaUnorm;
    case DDSPF_DXT2.fourCC:
    case DDSPF_DXT3.fourCC:
        return Bc2Unorm;
    case DDSPF_DXT4.fourCC:
    case DDSPF_DXT5.fourCC:
        return Bc3Unorm;
    case DDSPF_BC4_UNORM.fourCC:
        return Bc4Unorm;
    case DDSPF_BC4_SNORM.fourCC:
        return Bc4Snorm;
    case DDSPF_BC5_UNORM.fourCC:
        return Bc5Unorm;
    case DDSPF_BC5_SNORM.fourCC:
        return Bc5Snorm;
    case DDSPF_DX10.fourCC:
        {
            using enum DXGI_FORMAT;
            switch (header10.dxgiFormat)
            {
            case DXGI_FORMAT_BC1_UNORM:
                return Bc1RgbaUnorm;
            case DXGI_FORMAT_BC1_UNORM_SRGB:
                return Bc1RgbaSrgb;
            case DXGI_FORMAT_BC2_UNORM:
                return Bc2Unorm;
            case DXGI_FORMAT_BC2_UNORM_SRGB:
                return Bc2Srgb;
            case DXGI_FORMAT_BC3_UNORM:
                return Bc3Unorm;
            case DXGI_FORMAT_BC3_UNORM_SRGB:
                return Bc3Srgb;
            case DXGI_FORMAT_BC4_UNORM:
                return Bc4Unorm;
            case DXGI_FORMAT_BC4_SNORM:
                return Bc4Snorm;
            case DXGI_FORMAT_BC5_UNORM:
                return Bc5Unorm;
            case DXGI_FORMAT_BC5_SNORM:
                return Bc5Snorm;
            case DXGI_FORMAT_BC6H_UF16:
                return Bc6HUfloat;
            case DXGI_FORMAT_BC6H_SF16:
                return Bc6HSfloat;
            case DXGI_FORMAT_BC7_UNORM:
                return Bc7Unorm;
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                return Bc7Srgb;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
                return R8G8B8A8Unorm;
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                return R8G8B8A8Srgb;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return A2B10G10R10Unorm;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return R16G16B16A16SignedFloat;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                return B8G8R8A8Unrom;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                return B8G8R8A8Srgb;
            case DXGI_FORMAT_R32_FLOAT:
                return R32SignedFloat;
            case DXGI_FORMAT_R32G32_FLOAT:
                return R32G32SignedFloat;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                return R32G32B32A32SignedFloat;
            default:
                return Undefined;
            }
        }
    default:
        return Undefined;
    }
}
// https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide
uint64_t FTextureHeader::CalculateTextureImageSize(uint32_t width, uint32_t height, uint32_t depth,
                                                   uint32_t mipLevels, uint32_t blockSize, uint32_t blockDim)
{
    uint64_t res = 0;
    while (mipLevels--)
    {
        uint64_t blocksX = (uint64_t(width) + blockDim - 1) / blockDim;
        uint64_t blocksY = (uint64_t(height) + blockDim - 1) / blockDim;
        res += blocksX * blocksY * depth * blockSize;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
        depth = std::max(1u, depth / 2);
    }
    return res;
}
uint32_t FTextureHeader::GetBlockSize() const
{
    using enum RHIResourceFormat;
    switch (GetFormat())
    {
    case Bc1RgbUnorm:
    case Bc1RgbSrgb:
    case Bc1RgbaUnorm:
    case Bc1RgbaSrgb:
        return 8;
    case Bc2Unorm:
    case Bc2Srgb:
    case Bc3Unorm:
    case Bc3Srgb:
        return 16;
    case Bc4Unorm:
    case Bc4Snorm:
        return 8;
    case Bc5Unorm:
    case Bc5Snorm:
    case Bc6HUfloat:
    case Bc6HSfloat:
    case Bc7Unorm:
    case Bc7Srgb:
        return 16;
    default:
        return 0;
    }
}
uint32_t FTextureHeader::GetBpp() const
{
    using enum RHIResourceFormat;
    switch (GetFormat())
    {
    case R8G8B8A8Unorm:
    case R8G8B8A8Srgb:
    case B8G8R8A8Unrom:
    case B8G8R8A8Srgb:
    case A2R10G10B10Unorm:
    case A2R10G10B10Snorm:
    case A2B10G10R10Unorm:
    case A2B10G10R10Snorm:
        return 32;
    case R32SignedFloat:
        return 32;
    case R32G32SignedFloat:
        return 64;
    case R16G16B16A16SignedFloat:
        return 64;
    case R32G32B32A32SignedFloat:
        return 128;
    default:
        return 0;
    }
}
uint32_t FTextureHeader::GetSize() const
{
    uint32_t blockSize = GetBlockSize(), blockDim = 4;
    if (!blockSize)
        blockSize = GetBpp() / 8, blockDim = 1;
    uint64_t size = CalculateTextureImageSize(GetWidth(), GetHeight(), GetDepth(), GetNumMips(), blockSize, blockDim) *
                    GetNumLayers();
    CHECK_MSG(size <= std::numeric_limits<uint32_t>::max(), "Texture size {} exceeds addressable range", size);
    return static_cast<uint32_t>(size);
}
size_t FTextureHeader::GetSubresourceSize(uint32_t layer, uint32_t mip) const
{
    CHECK_MSG(layer < GetNumLayers(), "Texture array layer out of range: {} of {}", layer, GetNumLayers());
    CHECK_MSG(mip < GetNumMips(), "Texture mip level out of range: {} of {}", mip, GetNumMips());
    uint32_t blockSize = GetBlockSize(), blockDim = 4;
    if (!blockSize)
        blockSize = GetBpp() / 8, blockDim = 1;
    CHECK_MSG(blockSize && blockDim, "Unsupported texture format {}", GetFormat());
    RHIExtent3D mipExtent = GetMipExtent(mip);
    uint64_t mipSize = CalculateTextureImageSize(mipExtent.x, mipExtent.y, mipExtent.z, 1u, blockSize, blockDim);
    CHECK_MSG(mipSize <= std::numeric_limits<size_t>::max(),
              "Texture subresource size {} exceeds addressable range", mipSize);
    return static_cast<size_t>(mipSize);
}
RHIExtent3D FTextureHeader::GetMipExtent(uint32_t mipLevel) const
{
    return {
        std::max(1u, GetWidth() >> mipLevel),
        GetDimension() == RHITextureDimension::E1D ? 1u : std::max(1u, GetHeight() >> mipLevel),
        GetDimension() == RHITextureDimension::E3D ? std::max(1u, GetDepth() >> mipLevel) : 1u,
    };
}
RHITextureDesc FTextureHeader::GetDesc() const
{
    return RHITextureDesc{
        .resource = { .shared = true },
        .dimension = GetDimension(),
        .usage = RHITextureUsageBits::TransferDestination | RHITextureUsageBits::SampledImage,
        .extent = GetMipExtent(0),
        .format = GetFormat(),
        .mipLevels = GetNumMips(),
        .arrayLayers = GetDimension() == RHITextureDimension::E3D ? 1u : GetNumLayers()
    };
}
// https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide#using-texture-arrays-in-direct3d-1011
// [Layer 0 Mip 0][Layer 0 Mip 1]...[Layer 0 Mip N][Layer 1 Mip 0]...[Layer 1 Mip N]...
Span<unsigned char> FTexture::GetSubresource(uint32_t mipLevel, uint32_t arrayLayer) const
{
    uint32_t blockSize = GetBlockSize(), blockDim = 4;
    if (!blockSize)
        blockSize = GetBpp() / 8, blockDim = 1;
    CHECK_MSG(blockSize && blockDim, "Unsupported texture format {}", GetFormat());
    CHECK_MSG(arrayLayer < GetNumLayers(), "Texture array layer out of range: {} of {}", arrayLayer, GetNumLayers());
    CHECK_MSG(mipLevel < GetNumMips(), "Texture mip level out of range: {} of {}", mipLevel, GetNumMips());
    uint64_t layerOffset =
        uint64_t(arrayLayer) * CalculateTextureImageSize(GetWidth(), GetHeight(), GetDepth(), GetNumMips(), blockSize, blockDim);
    uint64_t mipOffset = CalculateTextureImageSize(GetWidth(), GetHeight(), GetDepth(), mipLevel, blockSize, blockDim);
    RHIExtent3D mipExtent = GetMipExtent(mipLevel);
    uint64_t mipSize = CalculateTextureImageSize(mipExtent.x, mipExtent.y, mipExtent.z, 1u, blockSize, blockDim);
    uint64_t offset = layerOffset + mipOffset;
    uint64_t offsetEnd = offset + mipSize;
    CHECK_MSG(offsetEnd <= bytes.size(), "Subresource out of range: layer {}, mip {} (size {}), data size {}",
              arrayLayer, mipLevel, mipSize, bytes.size());
    return {bytes.data() + offset, bytes.data() + offsetEnd};
}
void FTexture::GenerateMips()
{
    CHECK_MSG(GetNumMips() == 1, "Texture already has mipmaps");
    CHECK_MSG(GetFormat() == RHIResourceFormat::R8G8B8A8Unorm || GetFormat() == RHIResourceFormat::R8G8B8A8Srgb,
              "Source texture must be R8G8B8A8 format. Got {}", GetFormat());
    uint32_t numMips = std::max(GetWidth(), GetHeight());
    numMips = 1 + static_cast<uint32_t>(std::floor(std::log2(numMips)));
    if (numMips <= GetNumMips())
        return;
    RHIResourceFormat format = GetFormat();
    RHITextureDimension dimension = GetDimension();
    uint32_t width = GetWidth(), height = GetHeight(), depth = GetDepth(), layerCount = GetNumLayers();
    ddsCreateHeader(header, width, height, numMips, depth);
    ddsSetFormat(header, header10, layerCount, format, dimension);
    bytes.resize(GetSize());
    // Gamma correct. Mip generation should only be done in linear space.
    auto SrgbToLinear = [&](bool inverse = false /* linear to gamma */)
    {
        for (size_t i = 0; i < bytes.size(); i += 4)
        {
            float r = bytes[i + 0] / 255.0f;
            float g = bytes[i + 1] / 255.0f;
            float b = bytes[i + 2] / 255.0f;
            float a = bytes[i + 3] / 255.0f;
            // https://en.wikipedia.org/wiki/SRGB#Definition
            if (inverse)
            {
                r = (r <= 0.0031308f) ? (r * 12.92f) : (1.055f * std::pow(r, 1.0f / 2.4f) - 0.055f);
                g = (g <= 0.0031308f) ? (g * 12.92f) : (1.055f * std::pow(g, 1.0f / 2.4f) - 0.055f);
                b = (b <= 0.0031308f) ? (b * 12.92f) : (1.055f * std::pow(b, 1.0f / 2.4f) - 0.055f);
            }
            else
            {
                r = (r <= 0.04045f) ? (r / 12.92f) : std::pow((r + 0.055f) / 1.055f, 2.4f);
                g = (g <= 0.04045f) ? (g / 12.92f) : std::pow((g + 0.055f) / 1.055f, 2.4f);
                b = (b <= 0.04045f) ? (b / 12.92f) : std::pow((b + 0.055f) / 1.055f, 2.4f);
            }
            bytes[i + 0] = static_cast<unsigned char>(std::clamp(r * 255.0f, 0.0f, 255.0f));
            bytes[i + 1] = static_cast<unsigned char>(std::clamp(g * 255.0f, 0.0f, 255.0f));
            bytes[i + 2] = static_cast<unsigned char>(std::clamp(b * 255.0f, 0.0f, 255.0f));
            bytes[i + 3] = static_cast<unsigned char>(std::clamp(a * 255.0f, 0.0f, 255.0f));
        }
    };
    if (GetFormat() == RHIResourceFormat::R8G8B8A8Srgb)
        SrgbToLinear(false);
    for (uint32_t layer = 0; layer < GetNumLayers(); ++layer)
    {
        for (uint32_t mip = 1; mip < GetNumMips(); ++mip)
        {
            Span<const unsigned char> srcData = GetSubresource(mip - 1, layer);
            Span<unsigned char> dstData = GetSubresource(mip, layer);
            uint32_t srcWidth = std::max(1u, GetWidth() >> (mip - 1)),
                     srcHeight = std::max(1u, GetHeight() >> (mip - 1));
            const uint32_t mipWidth = std::max(1u, GetWidth() >> mip), mipHeight = std::max(1u, GetHeight() >> mip);
            for (uint32_t y = 0; y < mipHeight; ++y)
                for (uint32_t x = 0; x < mipWidth; ++x)
                {
                    uint32_t r = 0, g = 0, b = 0, a = 0;
                    uint32_t samples = 0;
                    for (uint32_t oy = 0; oy < 2; ++oy)
                        for (uint32_t ox = 0; ox < 2; ++ox)
                        {
                            uint32_t sx = x * 2 + ox;
                            uint32_t sy = y * 2 + oy;
                            if (sx < srcWidth && sy < srcHeight)
                            {
                                uint32_t srcIndex = (sy * srcWidth + sx) * 4;
                                r += srcData[srcIndex + 0];
                                g += srcData[srcIndex + 1];
                                b += srcData[srcIndex + 2];
                                a += srcData[srcIndex + 3];
                                samples++;
                            }
                        }
                    uint32_t dstIndex = (y * mipWidth + x) * 4;
                    dstData[dstIndex + 0] = static_cast<unsigned char>(r / samples);
                    dstData[dstIndex + 1] = static_cast<unsigned char>(g / samples);
                    dstData[dstIndex + 2] = static_cast<unsigned char>(b / samples);
                    dstData[dstIndex + 3] = static_cast<unsigned char>(a / samples);
                }
        }
    }
    if (GetFormat() == RHIResourceFormat::R8G8B8A8Srgb)
        SrgbToLinear(true);
}
void LoadDDS(FTexture& texture, StringView path)
{
    MemoryMappedFile file(path, MemoryMappedAccess::ReadOnly);
    MemoryReader reader(file.Bytes());
    FDeserialize(reader, texture);
}
#include <stb_image.h>
void LoadRGBA8(FTexture& texture, StringView path, bool gamma)
{
    int width, height, channels;
    stbi_uc* imgData = stbi_load(path.data(), &width, &height, &channels, STBI_rgb_alpha);
    UniquePtr<stbi_uc, decltype(&stbi_image_free)> raii(imgData, &stbi_image_free);
    CHECK_MSG(imgData != nullptr, "Failed to load image {}", path);

    texture.Initialize(gamma ? RHIResourceFormat::R8G8B8A8Srgb : RHIResourceFormat::R8G8B8A8Unorm,
                       RHITextureDimension::E2D, width, height);
    texture.bytes.assign(imgData, imgData + width * height * 4);
}
void LoadRGBA8(FTexture& texture, Span<const unsigned char> data, bool gamma)
{
    int width, height, channels;
    stbi_uc* imgData =
        stbi_load_from_memory(data.data(), data.size_bytes(), &width, &height, &channels, STBI_rgb_alpha);
    UniquePtr<stbi_uc, decltype(&stbi_image_free)> raii(imgData, &stbi_image_free);
    CHECK_MSG(imgData != nullptr, "Failed to load image from memory");

    texture.Initialize(gamma ? RHIResourceFormat::R8G8B8A8Srgb : RHIResourceFormat::R8G8B8A8Unorm,
                       RHITextureDimension::E2D, width, height);
    texture.bytes.assign(imgData, imgData + width * height * 4);
}
void LoadHDR(FTexture& texture, StringView path)
{
    int width = 0, height = 0;
    int channels = 0;
    float* imgData = stbi_loadf(path.data(), &width, &height, &channels, STBI_rgb_alpha);
    UniquePtr<float, decltype(&stbi_image_free)> raii(imgData, reinterpret_cast<void(*)(void*)>(&stbi_image_free));
    CHECK_MSG(imgData != nullptr, "Failed to load HDR image {}", path);

    texture.Initialize(RHIResourceFormat::R32G32B32A32SignedFloat, RHITextureDimension::E2D, width, height);
    const size_t size = width * height * 4 * sizeof(float);
    const auto* bytes = reinterpret_cast<const unsigned char*>(imgData);
    texture.bytes.assign(bytes, bytes + size);
}

void SaveHDR(const float* data, int width, int height, StringView path)
{
    // stbi_write_hdr expects RGB 3-channel data; extract from RGBA 4-channel input
    Vector<float> rgb(static_cast<size_t>(width) * height * 3, GLOBAL_ALLOC);
    for (int i = 0; i < width * height; ++i)
    {
        rgb[i * 3 + 0] = data[i * 4 + 0];
        rgb[i * 3 + 1] = data[i * 4 + 1];
        rgb[i * 3 + 2] = data[i * 4 + 2];
    }
    CHECK_MSG(stbi_write_hdr(path.data(), width, height, 3, rgb.data()),
              "Failed to write HDR image to {}", path);
}

void SavePNG(const unsigned char* data, int width, int height, StringView path)
{
    // stbi_write_png: RGBA 4-channel, stride = width * 4
    CHECK_MSG(stbi_write_png(path.data(), width, height, 4, data, width * 4),
              "Failed to write PNG image to {}", path);
}

FTexture FTexture::EncodeBC7(Allocator* alloc) const
{
    CHECK(alloc != nullptr);
    CHECK_MSG(GetFormat() == RHIResourceFormat::R8G8B8A8Unorm || GetFormat() == RHIResourceFormat::R8G8B8A8Srgb,
              "Source texture must be R8G8B8A8 format. Got {}", GetFormat());
    FTexture res(alloc);
    RHIResourceFormat dstFormat =
        (GetFormat() == RHIResourceFormat::R8G8B8A8Srgb) ? RHIResourceFormat::Bc7Srgb : RHIResourceFormat::Bc7Unorm;
    res.Initialize(dstFormat, GetDimension(), GetWidth(), GetHeight(), GetDepth(), GetNumMips(), GetNumLayers());
    res.bytes.resize(res.GetSize());

    bc7enc_compress_block_params pack_params;
    bc7enc_compress_block_params_init(&pack_params);
    for (uint32_t layer = 0; layer < GetNumLayers(); ++layer)
    {
        for (uint32_t mip = 0; mip < GetNumMips(); ++mip)
        {
            const uint32_t blockDim = 4;
            Span<const unsigned char> srcData = GetSubresource(mip, layer);
            const uint32_t mipWidth = std::max(1u, GetWidth() >> mip), mipHeight = std::max(1u, GetHeight() >> mip);
            const uint32_t blockX = (mipWidth + blockDim - 1) / blockDim,
                           blockY = (mipHeight + blockDim - 1) / blockDim;
            auto GetBlock = [&](uint32_t x, uint32_t y, unsigned char* block)
            {
                for (uint32_t by = 0; by < blockDim; ++by)
                    for (uint32_t bx = 0; bx < blockDim; ++bx)
                    {
                        uint32_t sx = x * blockDim + bx;
                        uint32_t sy = y * blockDim + by;
                        if (sx < mipWidth && sy < mipHeight)
                        {
                            uint32_t srcIndex = (sy * mipWidth + sx) * 4;
                            uint32_t dstIndex = (by * blockDim + bx) * 4;
                            block[dstIndex + 0] = srcData[srcIndex + 0];
                            block[dstIndex + 1] = srcData[srcIndex + 1];
                            block[dstIndex + 2] = srcData[srcIndex + 2];
                            block[dstIndex + 3] = srcData[srcIndex + 3];
                        }
                    }
            };
            Span<unsigned char> dstData = res.GetSubresource(mip, layer);
            for (uint32_t by = 0; by < blockY; ++by)
            {
                for (uint32_t bx = 0; bx < blockX; ++bx)
                {
                    const uint32_t blockSize = 16;
                    Array<unsigned char, 16 * 4> srcBlock;
                    GetBlock(bx, by, srcBlock.data());
                    Array<unsigned char, 16> compressedBlock;
                    bc7enc_compress_block(compressedBlock.data(), srcBlock.data(), &pack_params);
                    uint32_t dstIndex = (by * blockX + bx) * blockSize;
                    std::memcpy(dstData.data() + dstIndex, compressedBlock.data(), compressedBlock.size());
                }
            }
        }
    }
    return res;
}

