#include "KBMCamera.hpp"
#include <windowsx.h>
#include <hidusage.h>

void KBMCameraController::Win32RawInput_Setup(HWND hwnd)
{
    RAWINPUTDEVICE Rid[2];
    Rid[0].usUsagePage = Rid[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    Rid[0].dwFlags = Rid[1].dwFlags = RIDEV_INPUTSINK;
    Rid[0].hwndTarget = Rid[1].hwndTarget = hwnd;
    Rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
    Rid[1].usUsage = HID_USAGE_GENERIC_KEYBOARD;
    CHECK(RegisterRawInputDevices(Rid, ARRAYSIZE(Rid), sizeof(Rid[0])) && "RegisterRawInputDevices failed.");
}

void KBMCameraController::Win32_WndProcHandler(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message != WM_INPUT) return;
    static UINT dwSize = sizeof(RAWINPUT);
    static RAWINPUT rawInput;
    static BYTE lpb[sizeof(RAWINPUT)];
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &rawInput, &dwSize, sizeof(RAWINPUTHEADER));
    auto handle_mouse = [&](RAWINPUT& rawInput) {
        if (rawInput.data.mouse.usButtonFlags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) {
            // Scroll wheel event
            // Active when mouse is hovering on the viewport
            if (EditorGlobals::g_Editor.viewport.state == ViewportManipulationState::Nothing) return;
            Vector3 moveDirection = -translation;
            moveDirection.Normalize();
            float camAbsDistance = std::abs(translation.Length());
            // Move the mouse closer to the origin
            // 20 Notches per 10 increment in unit
            float unit = std::floor(std::log10(std::max(MIN_DISTANCE, camAbsDistance)));
            float delta = (pow(10, unit + 1) - pow(10, unit)) / 20.0f;
            float wheel = (float)(short)rawInput.data.mouse.usButtonData;
            if (wheel > 0 && camAbsDistance <= MIN_DISTANCE) return;
            if (wheel < 0 && camAbsDistance >= MAX_DISTANCE) return;
            translation += moveDirection * delta * (wheel > 0 ? 1 : -1);
        }
        {
            // Relative movement
            // Active only when the viewport is in UsingCamera state.
            LONG dX = rawInput.data.mouse.lLastX, dY = rawInput.data.mouse.lLastY;
            if (EditorGlobals::g_Editor.viewport.state == ViewportManipulationState::UsingCamera) {
                // Arcball camera control
                eulerRotation.y += dX * ROTATION_NOTCH;
                eulerRotation.x += dY * ROTATION_NOTCH;
            }
            else if (EditorGlobals::g_Editor.viewport.state == ViewportManipulationState::UsingCameraOffsetView) {
                // Moves viewpoint center
                AffineTransform T = get_transform();
                Vector3 Up = { 0,1,0 };
                Vector3 Front = { 0,0,1 };
                Up = Vector3::TransformNormal(Up, T);
                Front = Vector3::TransformNormal(Front, T);
                Vector3 Right = Up.Cross(Front);
                origin += dX * Right * OFFSET_NOTCH;
                origin += dY * Up * OFFSET_NOTCH;
            }
        }
    };
    auto handle_keyboard = [&](RAWINPUT& rawInput) {
        switch (rawInput.data.keyboard.VKey) {
        case ' ':
            reset();
            break;
        }
    };
    switch (rawInput.header.dwType) {
    case RIM_TYPEMOUSE:
        handle_mouse(rawInput);
        break;
    case RIM_TYPEKEYBOARD:
        handle_keyboard(rawInput);
        break;
    }
}