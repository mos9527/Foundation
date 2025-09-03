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
    enum class PassQueue {
        Graphics,
        Compute
    };

    class Renderer;
    using PassHandle = size_t;    
    class RenderPass : public RHIObject {
    public:
        /// <summary>
        /// Constructor. You may also create resources here for early setup.
        /// However, access declaration must be done in Setup().
        /// </summary>
        RenderPass() = default;
        /// <summary>
        /// Perform any setup required for this pass.
        /// This may include creating resources, declaring resource accesses, etc.
        /// </summary>        
        virtual void Setup(PassHandle self, Renderer&) = 0;
        /// <summary>
        /// Record the commands of this pass into the given command list.
        ///
        /// This is only executed after EndSetup() has been called,
        /// and when the render graph is actually executed.
        /// </summary>        
        virtual void Record(PassHandle self, Renderer&, RHICommandList*) = 0;
    };
}
