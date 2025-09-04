#include "Renderer.hpp"
#include "RenderPass.hpp"
namespace Foundation {
    struct CopyToSwapchainPass : public RenderPass {
        const ResourceHandle source{};
        CopyToSwapchainPass(ResourceHandle source) :
            source(source) {};
        void Setup(PassHandle self, Renderer& renderer) override;
        void Record(PassHandle self, Renderer&, RHICommandList* cmd) override;
    };
}
