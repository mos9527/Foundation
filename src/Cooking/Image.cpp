#include "Image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <Core/Platform/Logging.hpp>
using namespace Foundation::Cooking;
Cooked<Image> Cooker<Image>::sRGB32bpp_FromFile(std::filesystem::path const& path, Core::Allocator* allocator)
{
    int w, h, ch;
    stbi_uc* pixels = stbi_load(".derived/texture.jpg", &w, &h, &ch, STBI_rgb_alpha);
    ch = 4; // XXX: ch is the reported channel count - stb WILL load the image as RGBARGBA... row major
    CHECK(pixels && "Failed to load texture image");
    size_t size = 1ll * w * h * ch;
    Core::StlVector<char> data(pixels, pixels + size, allocator);
    stbi_image_free(pixels);
    return {
        .desc = {
            .extent = {w,h,1},
            .format = RHI::RHIResourceFormat::R8G8B8A8_UNORM,
        },
        .data = std::move(data)
    };
}
