#pragma once
#include <filesystem>
#include <Runtime/Blobs/Image.hpp>
#include <Runtime/Core/Allocator/StlContainers.hpp>
namespace Foundation::Cooking {
    using namespace Foundation::Blobs;
    using namespace Foundation::Core;

    template<typename T> class Cooker;
    template<typename T> class Cooked;

    template<> class Cooked<Image> {
        StlVector<char> data;
        RHI::RHIImageDesc desc{};
    public:   
        const Image GetImage() const {
            return {
                .desc = desc,
                .data = data.data()
            };
        }
        constexpr Image operator()() const { return GetImage(); }
    };

    template<> class Cooker<Image> {
    public:
        static Cooked<Image> sRGB32bpp_FromFile(std::filesystem::path const& path, Core::Allocator* allocator);
    };
}
