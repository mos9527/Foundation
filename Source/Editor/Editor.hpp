#pragma once
#include "../pch.hpp"
#include "Win32/Window.hpp"
#include "GlobalContext.hpp"

class EditorWindow : public BaseWindow {
	SceneCameraComponent* editorCamera;
	uint viewportWidth, viewportHeight;
	uint rendererFrameFlags{ 0 };
public:
	using BaseWindow::BaseWindow;
	virtual void WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	void Setup();
	void Run();
	void Destroy();
};
