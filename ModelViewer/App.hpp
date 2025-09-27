#pragma once
#include <Rendering/Application.hpp>
#include <Async/Thread.hpp>
#include <Rendering/PSFullscreen.hpp>
#include <RHIVulkan/Application.hpp>
#include "GPUScene.hpp"
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
        void OnBeforeFrame() override;
        void OnRendererSetup() override;
        void OnApplicationTick() override;
    public:
        UniquePtr<GPUScene> mScene;
        mat4 mCamera;
    };
}