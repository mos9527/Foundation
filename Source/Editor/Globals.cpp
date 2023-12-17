#include "Globals.hpp"
#include "Scene/SceneView.hpp"
#include "Processor/LTCFittedLUT.hpp"
#include "Processor/MeshPicking.hpp"
namespace EditorGlobals {    
    HWND g_Window;    
    RHIContext g_RHI;
    EditorContext g_Editor;
    SceneContext g_Scene;

    void SetupContext(HWND presentWindow, RHI::ResourceFormat presentFormat) {
        LOG(INFO) << "Setting up...";
        g_Window = presentWindow;
        g_RHI.device = new RHI::Device({ .AdapterIndex = 0 });
        g_RHI.swapchain = new RHI::Swapchain(g_RHI.device, { presentWindow, 1600, 1000, presentFormat });
        g_RHI.rootSig = new RHI::RootSignature(g_RHI.device, RHI::RootSignatureDesc()
            .SetDirectlyIndexed()
            .SetAllowInputAssembler()
            .AddConstantBufferView(0, 0) // Constant b0 space0 : Editor Global
            .AddConstantBufferView(1, 0) // Constant b1 space0 : Shader Global
            .AddConstant(2,0,8)          // Constant b2 space0 : 32-bit values * 8
            .AddStaticSampler(0, 0, RHI::SamplerDesc::GetAnisotropicSamplerDesc(16)) // Sampler s0 space0 : Texture Sampler
            .AddStaticSampler(1, 0, RHI::SamplerDesc::GetDepthSamplerDesc(       // Sampler s1 space0 : Depth Comp sampler
#ifdef INVERSE_Z
                true
#else
                false
#endif
            ))
            .AddStaticSampler(2, 0, RHI::SamplerDesc::GetDepthReduceSamplerDesc( // Sampler s2 space0 : Depth Reduce sampler
#ifdef INVERSE_Z
                true
#else
                false
#endif
            ))
            .AddStaticSampler(3,0, RHI::SamplerDesc::GetLinearSamplerDesc())
        );
        g_Scene.scene = new ::Scene;
        for (uint i = 0; i < ARRAYSIZE(g_Scene.views); i++) {
            g_Scene.views[i] = new ::SceneView(g_RHI.device);
        }
        g_RHI.data_LTCLUT = new ::LTCFittedLUT(g_RHI.device);
        g_Editor.viewport.meshPicker = new ::MeshPicking(g_RHI.device);
        LOG(INFO) << "Setup OK!";
    }    


    void DestroyContext(void) {
        /* SCENE */
        delete g_Scene.scene;
        for (uint i = 0; i < ARRAYSIZE(g_Scene.views); i++)
            delete g_Scene.views[i];
        /* EDITOR */
        delete g_Editor.viewport.meshPicker;
        /* DATA */
        delete g_RHI.data_LTCLUT;
        /* RHI OBJECTS */
        delete g_RHI.swapchain;
        delete g_RHI.rootSig;
        delete g_RHI.device;
    }
}
