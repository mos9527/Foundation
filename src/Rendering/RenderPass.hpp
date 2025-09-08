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
        RHIDeviceObjectHandle<RHITexture>,
        RHIBuffer*,
        RHITexture*
    >;
    using ResourceHandle = size_t; // Index in the resource definitions vector

    class Renderer;
    using PassHandle = size_t;
    /**
     * @brief Interface for a render pass.     
     */
    class RenderPass : public RHIObject {
    public:
        /**
         * @brief Constructor. You may also create resources here for early setup.
         * However, access declaration must be done in Setup().
         */
        RenderPass() = default;
        /**
         * @brief Perform any setup required for this pass.
         * This may include creating resources, declaring resource accesses, etc.
         */
        virtual void Setup(PassHandle self, Renderer* r) = 0;
        /**
         * @brief Record the commands of this pass into the given command list.
         *
         * This is only executed after EndSetup() has been called,
         * and when the render graph is actually executed.
         */
        virtual void Record(PassHandle self, Renderer* r, RHICommandList* cmd) = 0;
        /**
         * @brief Determine whether this pass should be skipped during Record time
         *
         * @return Whether this pass should be skipped during execution.
         */
        virtual bool IsSkipped(PassHandle self, Renderer* r) const { return false; }
    };
    /**
     * @brief Default "not skipped" functor
     */
    struct FSkipDefault {
        bool operator()(PassHandle, Renderer*) const { return false; }
    };
    /**
     * @brief Functional wrapper for a render pass
     *
     * This is a convenience wrapper for stateless passes, and should be created via @ref Renderer::CreatePass()
     */
    template<typename FSetup,typename FRecord,typename FSkip>
    struct LambdaPass : public RenderPass {
        FSetup m_setup;
        FRecord m_record;
        FSkip m_skip;
        LambdaPass(FSetup&& setup, FRecord&& record, FSkip&& skip = {})
            : m_setup(std::forward<FSetup>(setup)),
              m_record(std::forward<FRecord>(record)),
              m_skip(std::forward<FSkip>(skip)) {}
        void Setup(PassHandle self , Renderer* r) override {
            m_setup(self, r);
        }
        void Record(PassHandle self, Renderer* r, RHICommandList* cmd) override {
            m_record(self, r, cmd);
        }
        bool IsSkipped(PassHandle self, Renderer* r) const override {
            return m_skip(self, r);
        }
    };
}
