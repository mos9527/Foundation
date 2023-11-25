#include "GlobalContext.hpp"
RHI::Device* EditorGlobalContext::device;
RHI::Swapchain* EditorGlobalContext::swapchain;
Scene* EditorGlobalContext::scene;
SceneGraph* EditorGlobalContext::sceneGraph;

void EditorGlobalContext::SetupContext(HWND presentWindow, RHI::ResourceFormat presentFormat) {
    device = new RHI::Device({.AdapterIndex = 0});
    swapchain = new RHI::Swapchain(device, { presentWindow, 1600, 1000, presentFormat });
    scene = new ::Scene;
    sceneGraph = new ::SceneGraph(*scene);
    renderer = new ::DeferredRenderer(device);
    for (uint i = 0; i < ARRAYSIZE(sceneViews); i++) {
        sceneViews[i] = new ::SceneView(device);
    }
}

void EditorGlobalContext::DestroyContext() {
    delete device;
    delete swapchain;
    delete scene;
    delete sceneGraph;
    delete renderer;
    delete[] sceneViews;
}