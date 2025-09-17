#pragma once
#include <RHIVulkan/Application.hpp>
#include <Rendering/Application.hpp>
#include <Rendering/PSFullscreen.hpp>
#include <Async/Thread.hpp>
#include "Scene.hpp"
namespace Foundation::ModelViewer {
    using namespace Rendering;
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