#include "Context.hpp"
namespace EditorGlobals {    
    HWND g_Window;    
    RHIContext g_RHI;
    EditorContext g_Editor;

    void SetupContext(HWND presentWindow, RHI::ResourceFormat presentFormat) {
        LOG(INFO) << "Setting up...";
        g_Window = presentWindow;
        g_RHI.device = new RHI::Device({ .AdapterIndex = 0 });
        g_RHI.swapchain = new RHI::Swapchain(g_RHI.device, { presentWindow, 1600, 1000, presentFormat });
        g_RHI.rootSig = new RHI::RootSignature(g_RHI.device, RHI::RootSignatureDesc()
            .SetDirectlyIndexed()
            .SetAllowInputAssembler()
            .AddConstantBufferView(0, 0) // Constant b0 space0 : Editor Global
            .AddConstantBufferView(1, 0) // Constant b1 space0 : Shader Global (Optional)
            .AddStaticSampler(0, 0, RHI::SamplerDesc::GetTextureSamplerDesc(16)) // Sampler s0 space0 : Texture Sampler
            .AddStaticSampler(0, 0, RHI::SamplerDesc::GetDepthSamplerDesc(       // Sampler s1 space0 : Depth Comp sampler
#ifdef INVERSE_Z
                true
#else
                false
#endif
            ))
            .AddStaticSampler(0, 0, RHI::SamplerDesc::GetDepthReduceSamplerDesc( // Sampler s2 space0 : Depth Reduce sampler
#ifdef INVERSE_Z
                true
#else
                false
#endif
            )));
        LOG(INFO) << "Setup OK!";
    }    
}
