#pragma once
#include <filesystem>
#include <Runtime/Core/Core.hpp>
#include <RenderCore/RHICore/Resource.hpp>
using namespace Foundation;
struct Image {
    RHI::RHITextureDesc m_desc;
    Core::Allocator* m_allocator;
    Core::Vector<char> m_data;

    Image(RHI::RHITextureDesc const& desc, Core::Allocator* allocator, Core::Span<const char> data = {}) noexcept
        : m_desc(desc), m_allocator(allocator), m_data(data.begin(), data.end(), allocator) {
    }
    constexpr operator bool() const noexcept { return m_data.size() != 0; }
};

extern Image LoadImage32bppFromFile(std::filesystem::path const& path, Core::Allocator* allocator);

