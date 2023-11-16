#pragma once
#include "Win32/BaseWindow.hpp"
#include "../Common/Task.hpp"
class ViewportWindow : public BaseWindow {
private:
	WndCallbackFunction callback;
public:
	void Create(int width, int height, const wchar_t* szClassName, const wchar_t* szTitle) {
		BaseWindow::Create(width, height, szClassName, szTitle);		
	}
	void SetCallback(WndCallbackFunction&& cb) {
		callback = std::move(cb);
	}
	void WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
		if (callback)
			callback(hWnd, message, wParam, lParam);
		switch (message)
		{
		case WM_SIZE:			
			break;
		case WM_CLOSE:
			ExitProcess(0);
			break;
		}
		return;
	}	
};