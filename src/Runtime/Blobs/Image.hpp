#pragma once
#include "Blobs.hpp"
namespace Foundation::Blobs {  
    struct Image : public Blob {
        RHI::RHITextureDesc desc;
        Core::StlSpan<const char> data{ };
    public:
        Image(RHI::RHITextureDesc const& desc, Core::StlSpan<const char> data) noexcept
            : desc(desc), data(data) {}
        constexpr operator bool() const noexcept { return data.size();  }
        void Serialize(Stream& stream) override;
        void Deserialize(Stream& stream) override;
    };

}
