#pragma once
#include <cstdint>
#include <RHICore/Common.hpp>
#include <RHICore/Resource.hpp>
namespace Foundation::Blobs {
    // Densely packed image data structure.
    // Data layout MUST adhere to that of RHIImageDesc where applicable.    
    struct Image {
        RHI::RHIImageDesc desc;
        const char* data{ nullptr };
    };
}
