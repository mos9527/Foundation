#pragma once
#include <Async/Thread.hpp>
#include <RHIVulkan/Application.hpp>
#include <Rendering/Application.hpp>
#include <Rendering/PSFullscreen.hpp>

#include "Rendering/TexturePool.hpp"
#include "Scene/Scene.hpp"
#include "imgui.h"
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
        void OnRendererPostSetup() override;

        ResourceHandle mGBufferSRV{ kInvalidHandle };
        ImTextureID mGBufferHandle{};
    public:
        const RHIExtent3D kTextureMaxExtent{ 4096, 4096, 1 };
        RHIExtent3D mViewportSize{ 1280, 720, 1 };

        UniquePtr<Scene> mScene;
        UniquePtr<GPUScene> mGPUScene;
    };
}