#include "Texture.hpp"
#include <bc7enc.h>
using namespace Foundation::RHI;
FTexture2D::FTexture2D(Allocator* alloc) : magic(DDS_MAGIC), data(alloc)
{
    static bool bc7encInitialized = false;
    if (!bc7encInitialized)
        bc7enc_compress_block_init(), bc7encInitialized = true;
}
RHIResourceFormat FTexture2D::GetFormat() const
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
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                return B8G8R8A8Unrom;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                return B8G8R8A8Srgb;
            default:
                return Undefined;
            }
        }
    default:
        return Undefined;
    }
}
// https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide
uint32_t getImageSize(uint32_t width, uint32_t height, uint32_t mipLevels, uint32_t blockSize, uint32_t blockDim)
{
    uint32_t res = 0;
    while (mipLevels--)
    {
        res += ((width + blockDim - 1) / blockDim) * ((height + blockDim - 1) / blockDim) * blockSize;
        width = std::max(1u, width / 2), height = std::max(1u, height / 2);
    }
    return res;
}
uint32_t FTexture2D::GetBlockSize() const
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
uint32_t FTexture2D::GetBpp() const
{
    using enum RHIResourceFormat;
    switch (GetFormat())
    {
    case R8G8B8A8Unorm:
    case R8G8B8A8Srgb:
    case B8G8R8A8Unrom:
    case B8G8R8A8Srgb:
        return 32;
    default:
        return 0;
    }
}
uint32_t FTexture2D::GetSize() const
{
    uint32_t blockSize = GetBlockSize(), blockDim = 4;
    if (!blockSize)
        blockSize = GetBpp() / 8, blockDim = 1;
    return getImageSize(GetWidth(), GetHeight(), GetNumMips(), blockSize, blockDim) * GetNumLayers();
}
RHITextureDesc FTexture2D::GetDesc() const
{
    return RHITextureDesc{
        .resource = { .shared = true },
        .dimension = RHITextureDimension::E2D,
        .usage = RHITextureUsageBits::TransferDestination | RHITextureUsageBits::SampledImage,
        .extent = { GetWidth(), GetHeight(), 1 },
        .format = GetFormat(),
        .mipLevels = GetNumMips(),
        .arrayLayers = GetNumLayers()
    };
}
// https://learn.microsoft.com/en-us/windows/win32/direct3ddds/dx-graphics-dds-pguide#using-texture-arrays-in-direct3d-1011
// [Layer 0 Mip 0][Layer 0 Mip 1]...[Layer 0 Mip N][Layer 1 Mip 0]...[Layer 1 Mip N]...
Span<unsigned char> FTexture2D::GetSubresource(uint32_t mipLevel, uint32_t arrayLayer) const
{
    uint32_t blockSize = GetBlockSize(), blockDim = 4;
    if (!blockSize)
        blockSize = GetBpp() / 8, blockDim = 1;
    CHECK_MSG(blockSize && blockDim, "Unsupported texture format {}", GetFormat());
    uint32_t layerOffset = arrayLayer * getImageSize(GetWidth(), GetHeight(), GetNumMips(), blockSize, blockDim);
    uint32_t mipOffset = getImageSize(GetWidth(), GetHeight(), mipLevel, blockSize, blockDim);
    uint32_t mipWidth = std::max(1u, GetWidth() >> mipLevel), mipHeight = std::max(1u, GetHeight() >> mipLevel);
    uint32_t mipSize = getImageSize(mipWidth, mipHeight, 1u, blockSize, blockDim);
    uint32_t offset = layerOffset + mipOffset;
    uint32_t offsetEnd = offset + mipSize;
    CHECK_MSG(offsetEnd <= data.size(), "Subresource out of range: layer {}, mip {} (size {}), data size {}",
              arrayLayer, mipLevel, mipSize, data.size());
    return {data.data() + offset, data.data() + offsetEnd};
}
void FTexture2D::GenerateMips()
{
    CHECK_MSG(GetNumMips() == 1, "Texture already has mipmaps");
    CHECK_MSG(GetFormat() == RHIResourceFormat::R8G8B8A8Unorm || GetFormat() == RHIResourceFormat::R8G8B8A8Srgb,
              "Source texture must be R8G8B8A8 format. Got {}", GetFormat());
    uint32_t numMips = std::max(GetWidth(), GetHeight());
    numMips = 1 + static_cast<uint32_t>(std::floor(std::log2(numMips)));
    if (numMips <= GetNumMips())
        return;
    ddsCreateHeader(header, GetWidth(), GetHeight(), numMips);
    data.resize(GetSize());
    // Gamma correct. Mip generation should only be done in linear space.
    auto SrgbToLinear = [&](bool inverse = false /* linear to gamma */)
    {
        for (size_t i = 0; i < data.size(); i += 4)
        {
            float r = data[i + 0] / 255.0f;
            float g = data[i + 1] / 255.0f;
            float b = data[i + 2] / 255.0f;
            float a = data[i + 3] / 255.0f;
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
            data[i + 0] = static_cast<unsigned char>(std::clamp(r * 255.0f, 0.0f, 255.0f));
            data[i + 1] = static_cast<unsigned char>(std::clamp(g * 255.0f, 0.0f, 255.0f));
            data[i + 2] = static_cast<unsigned char>(std::clamp(b * 255.0f, 0.0f, 255.0f));
            data[i + 3] = static_cast<unsigned char>(std::clamp(a * 255.0f, 0.0f, 255.0f));
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
void LoadDDS(FTexture2D& texture, StringView path)
{
    FileReader reader(path);
    FDeserialize(reader, texture);
}
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
void LoadRGBA8(FTexture2D& texture, StringView path, bool gamma)
{
    int width, height, channels;
    stbi_uc* imgData = stbi_load(path.data(), &width, &height, &channels, STBI_rgb_alpha);
    UniquePtr<stbi_uc, decltype(&stbi_image_free)> raii(imgData, &stbi_image_free);
    CHECK_MSG(imgData != nullptr, "Failed to load image {}", path);

    ddsCreateHeader(texture.header, width, height, 1);
    ddsSetFormat(texture.header, texture.header10, 1,
                 gamma ? RHIResourceFormat::R8G8B8A8Srgb : RHIResourceFormat::R8G8B8A8Unorm);
    texture.data.assign(imgData, imgData + width * height * 4);
}
void LoadRGBA8(FTexture2D& texture, Span<const unsigned char> data, bool gamma)
{
    int width, height, channels;
    stbi_uc* imgData =
        stbi_load_from_memory(data.data(), data.size_bytes(), &width, &height, &channels, STBI_rgb_alpha);
    UniquePtr<stbi_uc, decltype(&stbi_image_free)> raii(imgData, &stbi_image_free);
    CHECK_MSG(imgData != nullptr, "Failed to load image from memory");

    ddsCreateHeader(texture.header, width, height, 1);
    ddsSetFormat(texture.header, texture.header10, 1,
                 gamma ? RHIResourceFormat::R8G8B8A8Srgb : RHIResourceFormat::R8G8B8A8Unorm);
    texture.data.assign(imgData, imgData + width * height * 4);
}
FTexture2D FTexture2D::EncodeBC7() const
{
    CHECK_MSG(GetFormat() == RHIResourceFormat::R8G8B8A8Unorm || GetFormat() == RHIResourceFormat::R8G8B8A8Srgb,
              "Source texture must be R8G8B8A8 format. Got {}", GetFormat());
    FTexture2D res(GLOBAL_ALLOC);
    ddsCreateHeader(res.header, GetWidth(), GetHeight(), GetNumMips());
    RHIResourceFormat dstFormat =
        (GetFormat() == RHIResourceFormat::R8G8B8A8Srgb) ? RHIResourceFormat::Bc7Srgb : RHIResourceFormat::Bc7Unorm;
    ddsSetFormat(res.header, res.header10, GetNumLayers(), dstFormat);
    res.data.resize(res.GetSize());

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
