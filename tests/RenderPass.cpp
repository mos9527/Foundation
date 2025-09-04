#include <stdio.h>
#include <vector>
#include <Core/Allocator/DefaultAllocator.hpp>
#include <Renderer/Renderer.hpp>

using namespace Foundation::Core;
using namespace Foundation;

DefaultAllocator g_Alloc;

class GBufferPass : public RenderPass {
public:
    ResourceHandle m_depth;
    GBufferPass(ResourceHandle depth) : m_depth(depth) {}
    void Setup(PassHandle self, Renderer& renderer) override {
        LOG_RUNTIME(GBufferPass, info, "Setup");
        renderer.BindTextureDSV(self, m_depth, {});
    }
    void Record(PassHandle self, Renderer& renderer, RHICommandList* cmdList) override {
        LOG_RUNTIME(GBufferPass, info, "Record!");
    }
};

class ShadowCascadePass : public RenderPass {
public:
    ResourceHandle m_shadowAtlas;
    int m_cascadeIndex;
    ShadowCascadePass(ResourceHandle atlas, int index) : m_shadowAtlas(atlas), m_cascadeIndex(index) {}
    void Setup(PassHandle self, Renderer& renderer) override {
        LOG_RUNTIME(ShadowCascadePass, info, "Setup");
        renderer.BindTextureRTV(self, m_shadowAtlas, {});
    }
    void Record(PassHandle self, Renderer& renderer, RHICommandList* cmdList) override {
        LOG_RUNTIME(ShadowCascadePass, info, "Record!");
    }
};

class CopyPass : public RenderPass {
public:
    ResourceHandle m_sourceDepth;
    ResourceHandle m_hiZBuffer;
    CopyPass(ResourceHandle source, ResourceHandle hiZ) : m_sourceDepth(source), m_hiZBuffer(hiZ) {}
    void Setup(PassHandle self, Renderer& renderer) override {
        LOG_RUNTIME(CopyPass, info, "Setup");
        renderer.BindTextureCopySrc(self, m_sourceDepth, {});
        renderer.BindTextureCopyDst(self, m_hiZBuffer, {});
    }
    void Record(PassHandle self, Renderer& renderer, RHICommandList* cmdList) override {
        LOG_RUNTIME(CopyPass, info, "Record!");
    }
};

class HiZDownsamplePass : public RenderPass {
public:
    ResourceHandle m_hiZBuffer; // The resource being mipped
    int m_mipLevel; // The mip level to generate
    HiZDownsamplePass(ResourceHandle hiZ, int mip) : m_hiZBuffer(hiZ), m_mipLevel(mip) {}
    void Setup(PassHandle self, Renderer& renderer) override {
        LOG_RUNTIME(HiZDownsamplePass, info, "Setup");
        renderer.BindTextureUAV(self, m_hiZBuffer, {});
    }
    void Record(PassHandle self, Renderer& renderer, RHICommandList* cmdList) override {
        LOG_RUNTIME(HiZDownsamplePass, info, "Record!");
    }
};

class LightingPass : public RenderPass {
public:
    ResourceHandle m_depth;
    ResourceHandle m_shadowAtlas;
    ResourceHandle m_hiZBuffer;
    ResourceHandle m_sceneColor;
    LightingPass(ResourceHandle d, ResourceHandle s, ResourceHandle h, ResourceHandle c)
        : m_depth(d), m_shadowAtlas(s), m_hiZBuffer(h), m_sceneColor(c) {
    }
    void Setup(PassHandle self, Renderer& renderer) override {
        LOG_RUNTIME(LightingPass, info, "Setup");
        renderer.BindTextureSRV(self, m_depth, {});
        renderer.BindTextureSRV(self, m_shadowAtlas, {});
        renderer.BindTextureSRV(self, m_hiZBuffer, {});
        renderer.BindTextureUAV(self, m_sceneColor, {});
    }
    void Record(PassHandle self, Renderer& renderer, RHICommandList* cmdList) override { /* Compute lighting... */ }
};

int main() {
    Renderer renderer(&g_Alloc);
    renderer.BeginSetup();
    auto sceneDepth = renderer.CreateResource("SceneDepth", RHITextureDesc{});
    auto shadowAtlas = renderer.CreateResource("ShadowAtlas", RHITextureDesc{});
    auto hiZBuffer = renderer.CreateResource("HiZBuffer", RHITextureDesc{});
    auto sceneColor = renderer.CreateResource("SceneColor", RHITextureDesc{});
    // HiZ generation is compute. This should be parallel and eventually sync'd
    renderer.CreatePass<CopyPass>("CopyHiZ0", RHIDevicePipelineType::Graphics, sceneDepth, hiZBuffer);
    const int NUM_MIPS = 8;
    for (int i = 1; i < NUM_MIPS; ++i)
        renderer.CreatePass<HiZDownsamplePass>(fmt::format("DownSample{}", i), RHIDevicePipelineType::Compute , hiZBuffer, i);
    // Gbuffer & cascade are both graphics work - these should be serial
    auto [_, gbufferPass] = renderer.CreatePass<GBufferPass>("GBuffer", RHIDevicePipelineType::Graphics, sceneDepth);
    const int NUM_CASCADES = 4;
    for (int i = 0; i < NUM_CASCADES; ++i)
        renderer.CreatePass<ShadowCascadePass>(fmt::format("Cascade{}", i), RHIDevicePipelineType::Graphics, shadowAtlas, i);
    // Epilouge lighting.
    // This should be the root for the topology, culling everything it's not using
    auto [lightingPassIdx, __] = renderer.CreatePass<LightingPass>("Lighting", RHIDevicePipelineType::Compute, sceneDepth, shadowAtlas, hiZBuffer, sceneColor);
    renderer.EndSetup(lightingPassIdx);
    printf(renderer.DbgDumpGraphviz().c_str());
    printf("\nPasses\n");
    printf(renderer.DbgDumpActivePasses().c_str());
    renderer.Execute();
}
