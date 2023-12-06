#include "../pch.hpp"
#include "Editor.hpp"
#include "Globals.hpp"

int main(int argc, char* argv[]) {
    FLAGS_alsologtostderr = true;    
    google::InitGoogleLogging(argv[0]);   
    
    CHECK(SetProcessDPIAware());
    CHECK(SetConsoleOutputCP(65001));    
    EditorWindow editor;
    editor.Create(1600, 1000, L"Editor", L"Foundation | Editor");
	EditorGlobalContext::SetupContext(editor.m_hWnd, RHI::ResourceFormat::R8G8B8A8_UNORM);
    editor.Setup();
    while (1) {
        MSG msg;
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            ZoneScopedN("Win32 Event");
            // Translate and dispatch the message
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            ZoneScopedN("Editor Run");
            editor.Run();
        }
    }
    editor.Destroy();
    EditorGlobalContext::DestroyContext();
}