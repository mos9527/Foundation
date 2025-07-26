#pragma once
#include "Common.hpp"
#include <Runtime/Blobs/Mesh.hpp>
namespace Foundation::Cooking {
    using enum RHI::RHIResourceFormat;            
    using OBJIndex = uint32_t; // 32-bit index
    struct OBJVertex {
        uint16_t px, py, pz; // quantized fp16
        uint16_t tp;         // tangent [octa 8+8]
        struct {
            uint32_t nx : 15;
            uint32_t ny : 15;
            uint32_t sign : 2;
        } np; // normal packed [snorm octa 15+15, bitangent sign 2]
        uint16_t u, v;       // texcoord fp16

    };
    // TODO: Remove this.
    // We will be doing GPU rendering which would render IA setup irrelavent
    static constexpr RHI::RHIVertexAttribute OBJAttributes[4]{
        {.location = 0, .binding = 0, .offset = offsetof(OBJVertex, px), .format = R16G16B16_SIGNED_FLOAT },
        {.location = 1, .binding = 0, .offset = offsetof(OBJVertex, tp), .format = R16_UINT },
        {.location = 2, .binding = 0, .offset = offsetof(OBJVertex, np), .format = R32_UINT},
        {.location = 3, .binding = 0, .offset = offsetof(OBJVertex, u),  .format = R16G16_SIGNED_FLOAT }
    };
    template<> class Cook<Mesh> {
    public:
        static Mesh FromOBJ(std::filesystem::path const& path, Core::Allocator* allocator);
    };
}
