#pragma once
#include "BaseWindow.hpp"
#include "Renderer/Renderer/Renderer.hpp"
#include "../Common/Task.hpp"
class ViewportWindow : public BaseWindow {
private:
	WORD width, height;
	std::unique_ptr<Renderer> renderer;
	task_threadpool<task_queue_container_lifo> taskpool;

public:
	void Create(int width, int height, const wchar_t* szClassName, const wchar_t* szTitle) {
		BaseWindow::Create(width, height, szClassName, szTitle);
		renderer = std::make_unique<Renderer>(
			Device::DeviceDesc{.AdapterIndex = 0},
			Swapchain::SwapchainDesc{.InitWidth = width, .InitHeight = height, .BackBufferCount = 2, .Window = m_hWnd }
		);		
		taskpool.push([&] {
			renderer->load_resources();
			LOG(INFO) << "Resource loaded....!";
		});
	}
	void WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
		switch (message)
		{
		case WM_SIZE:
			width = LOWORD(lParam);
			height = HIWORD(lParam);
			if (renderer.get())
				renderer->resize_viewport(width, height);
			break;
		case WM_CLOSE:
			ExitProcess(0);
			break;
		}
		return;
	}
	void Render() {
		if(renderer.get())
			renderer->render();
	}
};