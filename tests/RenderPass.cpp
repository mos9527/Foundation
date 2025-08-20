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
    void Setup(Renderer& renderer) override {
        LOG_RUNTIME(GBufferPass, info, "Setup");
        renderer.DeclareAccess(m_depth, ResourceAccess::Write);
    }
    void Record(RHICommandList* cmdList) override { /* Render scene to G-Buffer... */ }
};

class ShadowCascadePass : public RenderPass {
public:
    ResourceHandle m_shadowAtlas;
    int m_cascadeIndex;
    char m_name[32];
    ShadowCascadePass(ResourceHandle atlas, int index) : m_shadowAtlas(atlas), m_cascadeIndex(index) {
        snprintf(m_name, sizeof(m_name), "ShadowCascade_%d", index);
    }
    void Setup(Renderer& renderer) override {
        LOG_RUNTIME(ShadowCascadePass, info, "Setup");
        renderer.DeclareAccess(m_shadowAtlas, ResourceAccess::Write);
    }
    void Record(RHICommandList* cmdList) override { /* Render cascade depth... */ }
};

class CopyPass : public RenderPass {
public:
    ResourceHandle m_sourceDepth;
    ResourceHandle m_hiZBuffer;
    CopyPass(ResourceHandle source, ResourceHandle hiZ) : m_sourceDepth(source), m_hiZBuffer(hiZ) {}
    void Setup(Renderer& renderer) override {
        LOG_RUNTIME(CopyPass, info, "Setup");
        renderer.DeclareAccess(m_sourceDepth, ResourceAccess::Read);
        renderer.DeclareAccess(m_hiZBuffer, ResourceAccess::Write);
    }
    void Record(RHICommandList* cmdList) override { /* Copy depth to Hi-Z mip 0... */ }
};

class HiZDownsamplePass : public RenderPass {
public:
    ResourceHandle m_hiZBuffer; // The resource being mipped
    int m_mipLevel; // The mip level to generate
    char m_name[32];
    HiZDownsamplePass(ResourceHandle hiZ, int mip) : m_hiZBuffer(hiZ), m_mipLevel(mip) {
        snprintf(m_name, sizeof(m_name), "HiZ_Mip_%d", mip);
    }
    void Setup(Renderer& renderer) override {
        LOG_RUNTIME(HiZDownsamplePass, info, "Setup");
        renderer.DeclareAccess(m_hiZBuffer, ResourceAccess::ReadWrite);
    }
    void Record(RHICommandList* cmdList) override { /* Downsample previous mip... */ }
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
    void Setup(Renderer& renderer) override {
        LOG_RUNTIME(LightingPass, info, "Setup");
        renderer.DeclareAccess(m_depth, ResourceAccess::Read);
        renderer.DeclareAccess(m_shadowAtlas, ResourceAccess::Read);
        renderer.DeclareAccess(m_hiZBuffer, ResourceAccess::Read);
        renderer.DeclareAccess(m_sceneColor, ResourceAccess::Write);
    }
    void Record(RHICommandList* cmdList) override { /* Compute lighting... */ }
};

int main() {
    Renderer renderer(&g_Alloc);
    renderer.BeginSetup();
    auto sceneDepth = renderer.CreateResource("SceneDepth", RHITextureDesc{});
    auto shadowAtlas = renderer.CreateResource("ShadowAtlas", RHITextureDesc{});
    auto hiZBuffer = renderer.CreateResource("HiZBuffer", RHITextureDesc{});
    auto sceneColor = renderer.CreateResource("SceneColor", RHITextureDesc{});
    // Gbuffer & cascade are both graphics work - these should be serial
    auto [_, gbufferPass] = renderer.CreateGraphicsPass<GBufferPass>("Depth", sceneDepth);
    const int NUM_CASCADES = 4;
    for (int i = 0; i < NUM_CASCADES; ++i)
        auto [__, pass] = renderer.CreateGraphicsPass<ShadowCascadePass>(fmt::format("Cascade{}", i).c_str(), shadowAtlas, i);
    // HiZ generation is compute. This should be parallel and eventually sync'd
    renderer.CreateGraphicsPass<CopyPass>("CopyHiZ0", sceneDepth, hiZBuffer);
    const int NUM_MIPS = 8;
    for (int i = 1; i < NUM_MIPS; ++i)
        auto [__, pass] = renderer.CreateGraphicsPass<HiZDownsamplePass>(fmt::format("DownSample{}", i).c_str(), hiZBuffer, i);
    // Epilouge lighting.
    // This should be the root for the topology, culling everything it's not using
    renderer.CreateGraphicsPass<LightingPass>("Lighting", sceneDepth, shadowAtlas, hiZBuffer, sceneColor);
    renderer.EndSetup();
    // Output Graphviz
    printf("digraph G {\n");
    printf("    rankdir=TB;\n");
    auto& graph = renderer.m_setupContext->graph;
    auto& passes = renderer.m_renderPasses;
    auto& resources = renderer.m_resourceDefines;
    for (size_t u = 0; u < renderer.m_setupContext->graph.size(); u++) {
        for (auto [v, w] : graph[u]) {
            printf("    \"%s\" -> \"%s\" [label=\"%s\"];\n", passes[u].name, passes[v].name, resources[w].name);
        }
    }
    printf("}\n");
}
