#pragma once
#include "D3D12CommandQueue.hpp"
#include "D3D12DescriptorHeap.hpp"
#include "D3D12Types.hpp"
#include "D3D12Fence.hpp"
#include "D3D12CommandSignature.hpp"
#include "D3D12CommandList.hpp"
namespace RHI {	
	void LogDeviceInformation(ID3D12Device* device);
	void LogAdapterInformation(IDXGIAdapter1* adapter);
	void LogD3D12MABudget(D3D12MA::Allocator* allocator);
	void LogD3D12MAPoolStatistics(D3D12MA::Pool* pool);
	void CHECK_DEVICE_REMOVAL(Device* device, HRESULT hr);	
	class CommandList;
	class Resource;
	class Buffer;
	class Device {
	public:
		struct DeviceDesc {
			uint AdapterIndex{ 0 };
		};
		Device(DeviceDesc cfg);
		Device(Device&) = delete;
		Device(const Device&&) = delete;
		~Device();

		inline auto GetDXGIFactory() { return m_Factory.Get(); }
		inline auto GetNativeDevice() { return m_Device.Get(); } // TODO : Reduce the usage of GetNative*()
		
		void BeginCapture(const wchar_t* name);
		void EndCapture();		

		CommandQueue* GetCommandQueue(CommandListType type) {			
			if (type == CommandListType::Direct)return m_DirectQueue.get();
			if (type == CommandListType::Compute)return m_ComputeQueue.get();
			if (type == CommandListType::Copy)return m_CopyQueue.get();
			return nullptr;
		}

		template<CommandListType type> CommandQueue* GetCommandQueue() {			
			if constexpr (type == CommandListType::Direct)return m_DirectQueue.get();
			if constexpr (type == CommandListType::Compute)return m_ComputeQueue.get();
			if constexpr (type == CommandListType::Copy)return m_CopyQueue.get();
			return nullptr;
		}

		CommandList* GetDefaultCommandList(CommandListType type) {
			if (type == CommandListType::Direct)return m_DirectCmd.get();
			if (type == CommandListType::Compute)return m_ComputeCmd.get();
			if (type == CommandListType::Copy)return m_CopyCmd.get();
			return nullptr;
		}

		template<CommandListType type> CommandList* GetDefaultCommandList() {
			if constexpr (type == CommandListType::Direct)return m_DirectCmd.get();
			if constexpr (type == CommandListType::Compute)return m_ComputeCmd.get();
			if constexpr (type == CommandListType::Copy)return m_CopyCmd.get();
			return nullptr;
		}

		DescriptorHeap* GetDescriptorHeap(DescriptorHeapType type) {			
			if (type == DescriptorHeapType::CBV_SRV_UAV)return m_SRVHeap.get();
			if (type == DescriptorHeapType::DSV)return m_DSVHeap.get();
			if (type == DescriptorHeapType::RTV)return m_RTVHeap.get();
			if (type == DescriptorHeapType::SAMPLER)return m_SamplerHeap.get();
			return nullptr;
		}		

		template<DescriptorHeapType type> DescriptorHeap* GetDescriptorHeap() {		
			if constexpr (type == DescriptorHeapType::CBV_SRV_UAV)return m_SRVHeap.get();
			if constexpr (type == DescriptorHeapType::DSV)return m_DSVHeap.get();
			if constexpr (type == DescriptorHeapType::RTV)return m_RTVHeap.get();
			if constexpr (type == DescriptorHeapType::SAMPLER)return m_SamplerHeap.get();
			return nullptr;
		}

		OnlineDescriptorHeap* GetOnlineDescriptorHeap(DescriptorHeapType type) {
			if (type == DescriptorHeapType::CBV_SRV_UAV)return m_OnlineSRVHeap.get();
			return nullptr;
		}

		template<DescriptorHeapType type> OnlineDescriptorHeap* GetOnlineDescriptorHeap() {
			if constexpr (type == DescriptorHeapType::CBV_SRV_UAV)return m_OnlineSRVHeap.get();
			return nullptr;
		}
		auto* GetZeroBuffer() { return m_ZeroBuffer.get(); }
		void Wait();		
		inline auto GetAllocator() { return m_Allocator.Get(); }
		inline operator ID3D12Device* () { return m_Device.Get(); }		
	private:
		ComPtr<IDXGIAdapter1> m_Adapter;
		ComPtr<ID3D12Device5> m_Device;
		ComPtr<IDXGIFactory6> m_Factory;
		ComPtr<D3D12MA::Allocator> m_Allocator;		
		
		std::unique_ptr<Buffer>
			m_ZeroBuffer;
		std::unique_ptr<CommandQueue> 
			m_DirectQueue, 
			m_CopyQueue, 
			m_ComputeQueue;
		std::unique_ptr<CommandList>
			m_DirectCmd,
			m_CopyCmd,
			m_ComputeCmd;
		std::unique_ptr<OnlineDescriptorHeap> m_OnlineSRVHeap;
		std::unique_ptr<DescriptorHeap>
			m_RTVHeap,
			m_DSVHeap,
			m_SRVHeap,
			m_SamplerHeap;
	};
	
}