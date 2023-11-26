#pragma once
#include "Context.hpp"
namespace EditorGlobalContext {
    extern HWND window;

    extern RHI::Device* device;
    extern RHI::Swapchain* swapchain;
    extern DefaultTaskThreadPool taskpool;

    extern SceneContext scene;
    extern RenderContext render;
    extern EditorContext editor;
    extern ViewportContext viewport;

    extern void SetupContext(HWND presentWindow, RHI::ResourceFormat presentFormat);
    extern void DestroyContext();
};