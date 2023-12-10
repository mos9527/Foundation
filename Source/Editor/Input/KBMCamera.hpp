#pragma once
#include "../Globals.hpp"

constexpr float MIN_DISTANCE = 1e-3f;
constexpr float MAX_DISTANCE = 1e3f;
constexpr float ROTATION_NOTCH = 1e-1f * XM_2PI / 365;
constexpr float OFFSET_NOTCH = 1e-2f;
class KBMCameraController {
	Vector3 origin{};
	Vector3 translation{};
	Vector3 eulerRotation{};
public:
	bool active = true;
	AffineTransform get_transform() {
		AffineTransform T = XMMatrixTranslation(translation.x, translation.y, translation.z);
		T *= AffineTransform::CreateFromYawPitchRoll(eulerRotation);
		T *= XMMatrixTranslation(origin.x, origin.y, origin.z);
		return T;
	}
	void update_camera(SceneCameraComponent* camera) {		
		// Assuming camera's parent has only indentity transform:
		// Identity transform on camera yields Position{0,0,0} LookDirection{0,0,1} (+z)				
		if (active)
			camera->set_local_transform(get_transform());
	}
	void reset() {
		origin = { 0,0,0 };
		translation = { 0,1,-5 };
		eulerRotation = { XM_PIDIV4,0,0 };
	}
	void Win32RawInput_Setup(HWND hwnd);
	void Win32_WndProcHandler(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
};