#pragma once
#include "../Common.hpp"
#include "D3D12Swapchain.hpp"
#include "D3D12CommandQueue.hpp"
#include "D3D12DescriptorHeap.hpp"
#include "D3D12Texture.hpp"
#include "D3D12Resource.hpp"
#include "D3D12CommandList.hpp"
#include "D3D12Fence.hpp"
#define ALLOC_SIZE_DESC_HEAP 1024
namespace RHI {
	void LogDeviceInformation(ID3D12Device* device);
	void LogAdapterInformation(IDXGIAdapter1* adapter);
	void LogD3D12MABudget(D3D12MA::Allocator* allocator);
	class Device {
	public:
		struct DeviceDesc {
			UINT AdapterIndex{ 0 };
		};
		Device(DeviceDesc cfg);
		Device(Device&) = delete;
		Device(const Device&&) = delete;
		~Device();

		DescriptorHandle CreateRenderTargetView(Texture* tex);
		DescriptorHandle GetShaderResourceView(Buffer* buf, ResourceDimensionSRV view);
		DescriptorHandle GetConstantBufferView(Buffer* buf);

		std::shared_ptr<Buffer> AllocateIntermediateBuffer(Buffer::BufferDesc desc);
		void FlushIntermediateBuffers();

		void CreateSwapchainAndBackbuffers(Swapchain::SwapchainDesc const& cfg);

		void BeginFrame();
		void EndFrame(bool vsync);

		void WaitForGPU();

		inline auto GetDXGIFactory() { return m_Factory; }
		inline auto GetSwapchain() { return m_SwapChain.get(); }
		inline auto GetCommandQueue() { return m_CommandQueue.get(); }
		inline auto GetCommandList(CommandList::CommandListType type) { return m_CommandLists[type].get(); }
		inline auto GetCpuAllocator(DescriptorHeap::HeapType type) { return m_DescriptorHeaps[type].get(); }
		inline auto GetAllocator() { return m_Allocator.Get(); }

		inline auto GetNativeDevice() { return m_Device; }
		inline operator ID3D12Device* () { return m_Device.Get(); }
	private:
		ComPtr<IDXGIAdapter1> m_Adapter;
		ComPtr<ID3D12Device5> m_Device;
		ComPtr<IDXGIFactory6> m_Factory;
		ComPtr<D3D12MA::Allocator> m_Allocator;

		std::unique_ptr<Swapchain> m_SwapChain;
		std::unique_ptr<CommandQueue> m_CommandQueue;
		std::vector<std::unique_ptr<CommandList>> m_CommandLists;
		std::vector<std::unique_ptr<DescriptorHeap>> m_DescriptorHeaps;		
		std::vector<std::shared_ptr<Buffer>> m_IntermediateBuffers;
		std::unique_ptr<MarkerFence> m_MarkerFence;
	};
}