#pragma once
#include <filesystem>
#include <Runtime/Blobs/Image.hpp>
#include <Runtime/Core/Container/Common.hpp>

namespace Foundation::Cooking {
    using namespace Foundation::Blobs;
    using namespace Foundation::Core;

    template<typename T> class Cooker;
    template<typename T> class Cooked;

    template<> struct Cooked<Image> {
        RHI::RHITextureDesc desc{};    
        StlVector<char> data;
        const Image GetImage() const noexcept {
            return { desc, data };
        }
        const Image operator()() const noexcept { return GetImage(); }
    };

    template<> class Cooker<Image> {
    public:
        static Cooked<Image> sRGB32bpp_FromFile(std::filesystem::path const& path, Core::Allocator* allocator);
    };
}
