#pragma once
#include "../RHICommon.hpp"
#include "D3D12Swapchain.hpp"
#include "D3D12CommandQueue.hpp"
#include "D3D12DescriptorHeap.hpp"
#include "D3D12Texture.hpp"
#include "D3D12Resource.hpp"
#include "D3D12CommandList.hpp"
#include "D3D12Fence.hpp"
#define ALLOC_SIZE_DESC_HEAP 1024
namespace RHI {
	class Device {
	public:
		struct DeviceConfig {
			UINT AdapterIndex{ 0 };
		};
		Device(DeviceConfig cfg);
		Device(Device&) = delete;
		Device(const Device&&) = delete;
		~Device() = default;

		Descriptor CreateRenderTargetView(Texture* tex);
		void CreateSwapchainAndBackbuffers(Swapchain::SwapchainConfig const& cfg);

		void BeginFrame();
		void EndFrame();

		void WaitForGPU();

		inline auto GetNativeDevice() { return m_Device; }
		inline auto GetSwapchain() { return m_SwapChain.get(); }
		inline auto GetCommandQueue() { return m_CommandQueue.get(); }
		inline auto GetCommandList(CommandList::CommandListType type) { return m_CommandLists[type].get(); }
		inline auto GetCpuAllocator(DescriptorHeap::HeapType type) { return m_DescriptorHeapAllocators[type].get(); }

		inline auto GetDXGIFactory() { return m_Factory; }

		inline operator ID3D12Device* () { return m_Device.Get(); }
	private:
		ComPtr<ID3D12Device5> m_Device;
		ComPtr<IDXGIFactory6> m_Factory;
		std::unique_ptr<Swapchain> m_SwapChain;
		std::unique_ptr<CommandQueue> m_CommandQueue;
		std::vector<std::unique_ptr<CommandList>> m_CommandLists;
		std::vector<std::unique_ptr<DescriptorHeapAllocator>> m_DescriptorHeapAllocators;
		
		std::unique_ptr<MarkerFence> m_MarkerFence;
	};
}