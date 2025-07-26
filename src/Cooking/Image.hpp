#pragma once
#include "Common.hpp"
#include <Runtime/Blobs/Image.hpp>
namespace Foundation::Cooking {
    template<> class Cook<Image> {
    public:
        static Image sRGB32bpp_FromFile(std::filesystem::path const& path, Core::Allocator* allocator);
    };
}
