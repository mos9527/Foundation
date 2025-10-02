#pragma once
#include <Async/Thread.hpp>
#include <RHIVulkan/Application.hpp>
#include <Rendering/Application.hpp>
#include <Rendering/PSFullscreen.hpp>
#include "Assets/Scene.hpp"
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
        void OnDeviceSetup() override;        
        void OnRendererSetup() override;        
        void OnApplicationTick() override;
        void OnBeforeFrame() override;
    public:
        UniquePtr<Scene> mScene;
        UniquePtr<GPUScene> mGPUScene;
    };
}