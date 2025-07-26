#pragma once
#include "Blobs.hpp"
namespace Foundation::Blobs {  
    struct Image : public Blob {
        RHI::RHITextureDesc m_desc;
        Core::Allocator* m_allocator;
        Core::StlVector<char> m_data;

        Image(RHI::RHITextureDesc const& desc, Core::Allocator* allocator, Core::StlSpan<const char> data = {}) noexcept
            : m_desc(desc), m_allocator(allocator), m_data(data.begin(), data.end(), allocator) {}
        constexpr operator bool() const noexcept { return m_data.size();  }

        void Serialize(Stream& stream) override;
        void Deserialize(Stream& stream) override;
    };

}
