#pragma once
#include "../../pch.hpp"
typedef void (*WndCallbackPtr) (HWND, UINT, WPARAM, LPARAM);
typedef std::function<void(HWND, UINT, WPARAM, LPARAM)> WndCallbackFunction;
class BaseWindow {          
public:
    HWND m_hWnd{};
    virtual void WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) = 0;
    BaseWindow(int width, int height, const wchar_t* szClassName, const wchar_t* szTitle) {
        RECT rect{};
        SetRect(&rect, 0, 0, width,height);
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, false); // Calculate viewport size from client size        
        // Register window class
        WNDCLASSEX windowClass = { 0 };
        windowClass.cbSize = sizeof(WNDCLASSEX);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = StaticWndProc;
        windowClass.hInstance = NULL;
        windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
        windowClass.lpszClassName = szClassName;
        RegisterClassEx(&windowClass);        
        // Create the window
        m_hWnd = CreateWindowW(
            windowClass.lpszClassName,
            szTitle,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            (rect.right - rect.left), (rect.bottom - rect.top),
            NULL,
            NULL,
            NULL,
            this
        );        
        LOG_SYSRESULT(GetLastError());
        CHECK(m_hWnd != NULL);
        ShowWindow(
            m_hWnd,
            SW_SHOWNORMAL
        );
    }

private:
    static LRESULT CALLBACK StaticWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_CREATE) {
            // lParam contains pointer to class instance
            LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
            SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
            return 0;
        }
        BaseWindow* This = reinterpret_cast<BaseWindow*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
        if (This)
            This->WndProc(hWnd, message, wParam, lParam);                
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
};