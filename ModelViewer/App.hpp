#pragma once
#include <Async/Thread.hpp>
#include <RHIVulkan/Application.hpp>
#include <Rendering/Application.hpp>
#include <Rendering/PSFullscreen.hpp>
#include "Scene/Scene.hpp"
/**
 * @brief ModelViewer implementation
 */
namespace ModelViewer {
    using namespace Foundation;
    using namespace Foundation::Rendering;
    /**
     * @brief Model Viewer Application implementation
     */
    class App : public RenderApplication {
        void OnImGui();

        void OnDeviceSetup() override;        
        void OnRendererSetup() override;        
        void OnApplicationTick() override;
        void OnBeforeFrame() override;
        void OnAfterFrame() override;
    public:
        UniquePtr<Scene> mScene;
        UniquePtr<GPUScene> mGPUScene;
    };
}