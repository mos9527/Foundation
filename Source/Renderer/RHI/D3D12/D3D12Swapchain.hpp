#pragma once
#include "../Common.hpp"
#include "D3D12Texture.hpp"
#include "D3D12Fence.hpp"
#include "D3D12CommandQueue.hpp"
#include "D3D12DescriptorHeap.hpp"
namespace RHI {
	class Device;
	class Swapchain : public RHIObject {
		using RHIObject::m_Name;
		uint nBackbufferIndex{ 0 };	 /* The backbuffer we are currently drawing to */
		std::vector<size_t> nFenceValues;
		std::unique_ptr<Fence> m_FrameFence;
		std::vector<std::unique_ptr<Texture>> m_Backbuffers;
		std::vector<std::shared_ptr<Descriptor>> m_BackbufferRTVs;
		ComPtr<IDXGISwapChain3> m_Swapchain;
		void Present(bool vsync);
	public:
		BOOL bIsFullscreen{ false };
		struct SwapchainDesc {
			int InitWidth;
			int InitHeight;
			uint BackBufferCount;
			HWND Window;
		};
		Swapchain(Device* device, CommandQueue* cmdQueue, SwapchainDesc const& cfg);
		~Swapchain() = default;
		
		inline auto GetBackbuffer(uint index) { return m_Backbuffers[index].get(); }
		inline auto GetBackbufferRTV(uint index) { return m_BackbufferRTVs[index]; }

		inline auto GetCurrentBackbuffer() { return nBackbufferIndex; }
		void CreateBackBuffers(Device* device);
		void CreateRenderTargetViews(Device* device);
		void Resize(Device* device, CommandQueue* cmdQueue, uint width, uint height);
		void PresentAndMoveToNextFrame(CommandQueue* signalQueue, bool vsync);

		inline auto GetNativeSwapchain() { return m_Swapchain.Get(); }
		inline operator IDXGISwapChain3* () { return m_Swapchain.Get(); }

		using RHIObject::GetName;
		inline void SetName(name_t name) {
			m_Name = name;			
		}
	};
}