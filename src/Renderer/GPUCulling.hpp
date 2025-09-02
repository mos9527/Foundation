#pragma once
#include "RenderPass.hpp"
namespace Foundation {
    struct GPUCulling : public RenderPass {
        const size_t kMaxCommandBytes = 1LL << 20; // 1M        
        ResourceHandle
            /* ScenePass data buffers */
            sceneGlobal, sceneInstance, scenePrimitive,
            // (TODO UNUSED) Hierarchical Z-Buffer texture
            hizDepth{ kInvalidHandle },
            // Output instance draw commands buffer            
            m_cmds{ kInvalidHandle },
            // Atomic command counter (uint64)
            m_ctr{ kInvalidHandle };
        GPUCulling(Renderer& renderer, ResourceHandle sceneGlobal, ResourceHandle sceneInstance, ResourceHandle scenePrimitive);
        virtual void Setup(PassHandle self, Renderer& renderer) override;
        virtual void Record(PassHandle self, Renderer&, RHICommandList* cmd);
    };
}
