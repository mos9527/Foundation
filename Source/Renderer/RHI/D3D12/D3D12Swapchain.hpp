#pragma once
#include "../Common.hpp"
#include "D3D12Texture.hpp"
#include "D3D12Fence.hpp"
#include "D3D12CommandQueue.hpp"
#include "D3D12DescriptorHeap.hpp"
namespace RHI {
	class Device;
	class Swapchain {
		size_t nFenceValue;
		std::vector<size_t> nFenceValues;
		std::unique_ptr<Fence> m_FrameFence;
		std::vector<std::unique_ptr<Texture>> m_Backbuffers;
		std::vector<DescriptorHandle> m_BackbufferRTVs;
		ComPtr<IDXGISwapChain3> m_Swapchain;
		void Present(bool vsync);
	public:
		/* The backbuffer we are currently drawing to */
		uint nBackbufferIndex{ 0 };
		BOOL bIsFullscreen{ false };
		struct SwapchainDesc {
			int InitWidth;
			int InitHeight;
			uint BackBufferCount;
			HWND Window;
		};
		Swapchain(Device* device, SwapchainDesc const& cfg);
		~Swapchain() = default;
		
		inline auto GetBackbuffer(uint index) { return m_Backbuffers[index].get(); }
		inline auto GetBackbufferRTV(uint index) { return m_BackbufferRTVs[index]; }

		void CreateBackBuffers(Device* device);
		void CreateRenderTargetViews(Device* device);
		void Resize(Device* device, uint width, uint height);
		void PresentAndMoveToNextFrame(CommandQueue* signalQueue, bool vsync);

		inline auto GetNativeSwapchain() { return m_Swapchain.Get(); }
		inline operator IDXGISwapChain3* () { return m_Swapchain.Get(); }
	};
}