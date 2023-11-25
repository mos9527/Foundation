#include "../pch.hpp"
#include "Editor.hpp"
#include "GlobalContext.hpp"

int main() {
	EditorWindow editor(1600,900,L"Editor",L"Foundation | Editor");
	EditorGlobalContext::SetupContext(editor.m_hWnd, RHI::ResourceFormat::R8G8B8A8_UNORM);
    editor.Setup();
    while (1) {
        MSG msg;
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            // Translate and dispatch the message
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            editor.Run();
        }
    }
    editor.Destroy();
    EditorGlobalContext::DestroyContext();
}