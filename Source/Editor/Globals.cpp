#include "Context.hpp"
namespace EditorGlobalContext {
    HWND window;

    RHI::Device* device;
    RHI::Swapchain* swapchain;
    DefaultTaskThreadPool taskpool;

    SceneContext scene;
    RenderContext render;
    EditorContext editor;
    ViewportContext viewport;

    void SetupContext(HWND presentWindow, RHI::ResourceFormat presentFormat) {
        window = presentWindow;
        device = new RHI::Device({ .AdapterIndex = 0 });
        swapchain = new RHI::Swapchain(device, { presentWindow, 1600, 1000, presentFormat });
        scene.scene = new ::Scene;
        render.renderer = new ::DeferredRenderer(device);
        editor.iblProbe = new ::IBLProbeProcessor(device, 512);
        editor.ltcTable = new ::LTCTableProcessor(device);
        editor.meshSelection = new ::MeshSelectionProcessor(device);
        for (uint i = 0; i < ARRAYSIZE(scene.views); i++) {
            scene.views[i] = new ::SceneView(device);
        }        
    }

    void DestroyContext(void) {
        delete scene.scene;
        for (uint i = 0; i < ARRAYSIZE(scene.views); i++)
            delete scene.views[i];
        delete render.renderer;
        delete swapchain;
        delete device;
    }
}