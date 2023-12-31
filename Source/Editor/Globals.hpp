#pragma once
#include "Context.hpp"

namespace EditorGlobals {
    extern HWND g_Window;
    extern RHIContext g_RHI;
    extern EditorContext g_Editor;
    extern SceneContext g_Scene;

    extern void SetupContext(HWND presentWindow, RHI::ResourceFormat presentFormat);
    extern void DestroyContext();
};