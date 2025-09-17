#pragma once
#include <Rendering/Application.hpp>
#include <Runtime/Async/Thread.hpp>
#include <Rendering/PSFullscreen.hpp>
#include <RenderCore/RHIVulkan/Application.hpp>
#include "Scene.hpp"
namespace Foundation::ModelViewer {
    using namespace RenderCore;
    /**
     * @brief Model Viewer Application implementation
     */
    class App : public RenderApplication {
        void OnDeviceSetup() override;
        void OnBeforeFrame() override;
        /**
         * @brief Implements a GPU-driven render graph in @ref Render.cpp
         */
        void OnRendererSetup() override;
        void OnApplicationTick() override;
    public:
        UniquePtr<Scene> m_scene;
        mat4 m_camera;
        Vector<SceneFuture> m_meshes;

        App() : m_meshes(GetAllocator()) {};
    };
}