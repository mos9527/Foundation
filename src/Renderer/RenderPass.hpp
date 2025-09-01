#pragma once
#include <RHICore/Command.hpp>
#include <Bits/Functional.hpp>
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using ResourceDefinition = Variant<        
        RHIBufferDesc,
        RHITextureDesc,
        RHIDeviceObjectHandle<RHIBuffer>,
        RHIDeviceObjectHandle<RHITexture>
    >;
    using ResourceHandle = size_t; // Index in the resource definitions vector
    enum class PassType {
        Graphics,
        Compute
    };

    class Renderer;
    using PassHandle = size_t;
    class RenderPass : public RHIObject {
    public:
        virtual void Setup(PassHandle self, Renderer&) = 0;
        virtual void Record(PassHandle self, Renderer&, RHICommandList*) = 0;
        virtual RHIPipelineStage GetPipelineStage() const = 0;
    };
}

