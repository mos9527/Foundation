#pragma once
#include "Win32/Window.hpp"
#include "Globals.hpp"

class EditorWindow : public BaseWindow {
public:
	using BaseWindow::BaseWindow;
	virtual void WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
	void Setup();
	void Run();
	void Destroy();
};
