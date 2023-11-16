#pragma once
#include "D3D12Types.hpp"
#include "D3D12Resource.hpp"
#include "D3D12Fence.hpp"
#include "D3D12CommandQueue.hpp"
#include "D3D12DescriptorHeap.hpp"
#include "D3D12ResourceView.hpp"
namespace RHI {
	class Device;
	class Swapchain : public DeviceChild {
		name_t m_Name;
		BOOL bIsFullscreen{ false };
		uint nBackbufferIndex{ 0 };	 /* The backbuffer we are currently drawing to */
		uint nFrameIndex{ 0 };
		std::vector<size_t> nFenceValues;
		std::unique_ptr<Fence> m_FrameFence;
		std::vector<std::unique_ptr<Texture>> m_Backbuffers;
		std::vector<RenderTargetView> m_BackbufferRTVs;
		ComPtr<IDXGISwapChain3> m_Swapchain;

		ResourceFormat m_BackbufferFormat;
		uint nWidth, nHeight;
		void Present(bool vsync);		
	public:
		struct SwapchainDesc {
			HWND Window;
			int InitWidth;
			int InitHeight;
			ResourceFormat Format;
			uint BackBufferCount = RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT;
		};
		Swapchain(Device* device, SwapchainDesc const& cfg);
		~Swapchain() = default;
		
		inline auto GetBackbuffer(uint index) { return m_Backbuffers[index].get(); }
		inline auto GetBackbufferRTV(uint index) { return m_BackbufferRTVs[index]; }
		inline auto GetCurrentBackbufferIndex() const { return nBackbufferIndex; }
		inline auto GetBackbufferCount() const { return m_Backbuffers.size(); }
		inline auto GetFrameIndex() const { return nFrameIndex; }
		inline auto GetFormat() const { return m_BackbufferFormat; }
		inline auto GetWidth() const { return nWidth; }
		inline auto GetAspect() const { return nWidth / (float)nHeight; }
		inline auto GetHeight() const { return nHeight; }

		void Resize( uint width, uint height);
		void PresentAndMoveToNextFrame(bool vsync);

		inline auto GetNativeSwapchain() { return m_Swapchain.Get(); }
		inline operator IDXGISwapChain3* () { return m_Swapchain.Get(); }

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;			
		}
	};
}