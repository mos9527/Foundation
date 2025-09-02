#pragma once
#include "Renderer.hpp"
#include "RenderPass.hpp"
namespace Foundation {
    struct GBuffer : public RenderPass {
        ResourceHandle
            /* ScenePass data buffers */
            sceneGlobal, sceneInstance, scenePrimitive,
            // GBuffers
            // These are declared as soon as the pass is constructed/created
            m_albedo{ kInvalidHandle }, m_depth{ kInvalidHandle },
            // GBuffer Views
            m_albedoView{ kInvalidHandle }, m_depthView{ kInvalidHandle };
        GBuffer(Renderer& renderer, ResourceHandle sceneGlobal, ResourceHandle sceneInstance, ResourceHandle scenePrimitive);
        virtual void Setup(PassHandle self, Renderer& renderer) override;
        virtual void Record(PassHandle self, Renderer&, RHICommandList* cmd) override;
    };
}
