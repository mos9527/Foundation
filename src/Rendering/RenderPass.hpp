#pragma once
#include <RHICore/Command.hpp>
#include <Bits/Functional.hpp>
namespace Foundation::Rendering {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using ResourceDefinition = Variant<        
        RHIBufferDesc,
        RHITextureDesc,
        RHIDeviceObjectHandle<RHIBuffer>,
        RHIDeviceObjectHandle<RHITexture>
    >;
    using ResourceHandle = size_t; // Index in the resource definitions vector

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
        virtual void Setup(PassHandle self, Renderer* r) = 0;
        /// <summary>
        /// Record the commands of this pass into the given command list.
        ///
        /// This is only executed after EndSetup() has been called,
        /// and when the render graph is actually executed.
        /// </summary>        
        virtual void Record(PassHandle self, Renderer* r, RHICommandList* cmd) = 0;
    };
    /// <summary>
    /// Functional wrapper for a render pass    
    /// </summary>    
    template<typename FSetup, typename FRecord>
    struct LambdaPass : public RenderPass {
        FSetup m_setup;
        FRecord m_record;
        LambdaPass(FSetup&& setup, FRecord&& record)
            : m_setup(std::forward<FSetup>(setup)), m_record(std::forward<FRecord>(record)) {
        }
        virtual void Setup(PassHandle self , Renderer* r) override {
            m_setup(self, r);
        }
        virtual void Record(PassHandle self, Renderer* r, RHICommandList* cmd) override {
            m_record(self, r, cmd);
        }
    };
}
