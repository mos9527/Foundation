#pragma once
#include "../Runtime/RHI/RHI.hpp"
#include "../Runtime/Asset/AssetRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneView.hpp"
#include "Scene/SceneGraph.hpp"
#include "Renderer/Deferred.hpp"

struct EditorGlobalContext {
    static RHI::Device* device;
    static RHI::Swapchain* swapchain;    

    static Scene* scene;
    static SceneView* sceneViews[RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT];
    static SceneGraph* sceneGraph;
    static DeferredRenderer* renderer;

    static std::mutex renderMutex;
    static std::mutex loadMutex;

    static void SetupContext(HWND presentWindow, RHI::ResourceFormat presentFormat);
    static void DestroyContext();
};