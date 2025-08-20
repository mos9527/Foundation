#pragma once
#include <RHICore/Command.hpp>
#include <variant>
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using ResourceDefinition = std::variant<RHIBufferDesc, RHITextureDesc>;   
    enum class ResourceAccess {
        Read,
        Write,
        ReadWrite
    };
    using ResourceHandle = size_t; // Index in the resource definitions vector
    enum class PassType {
        Graphics,
        Compute
    };

    class Renderer;
    class RenderPass : public RHIObject {
    public:
        virtual void Setup(Renderer&) = 0;
        virtual void Record(RHICommandList*) = 0;
    };
}

