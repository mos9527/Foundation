#pragma once
#include "D3D12CommandQueue.hpp"
#include "D3D12DescriptorHeap.hpp"
#include "D3D12Resource.hpp"
#include "D3D12Types.hpp"
#include "D3D12CommandList.hpp"
#include "D3D12Fence.hpp"
#include "D3D12CommandSignature.hpp"

namespace RHI {	
	void LogDeviceInformation(ID3D12Device* device);
	void LogAdapterInformation(IDXGIAdapter1* adapter);
	void LogD3D12MABudget(D3D12MA::Allocator* allocator);
	void LogD3D12MAPoolStatistics(D3D12MA::Pool* pool);
	void CHECK_DEVICE_REMOVAL(Device* device, HRESULT hr);	
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
		
		CommandQueue* GetCommandQueue(CommandListType type) {
			using CommandListType;
			if (type == Direct)return m_DirectQueue.get();
			if (type == Compute)return m_ComputeQueue.get();
			if (type == Copy)return m_CopyQueue.get();
			return nullptr;
		}

		template<CommandListType type> CommandQueue* GetCommandQueue() {
			using CommandListType;
			if constexpr (type == Direct)return m_DirectQueue.get();
			if constexpr (type == Compute)return m_ComputeQueue.get();
			if constexpr (type == Copy)return m_CopyQueue.get();
			return nullptr;
		}

		DescriptorHeap* GetDescriptorHeap(DescriptorHeapType type) {
			using DescriptorHeapType;
			if (type == CBV_SRV_UAV)return m_SRVHeap.get();
			if (type == DSV)return m_DSVHeap.get();
			if (type == RTV)return m_RTVHeap.get();
			if (type == SAMPLER)return m_SamplerHeap.get();
			return nullptr;
		}		

		template<DescriptorHeapType type> DescriptorHeap* GetDescriptorHeap() {
			using DescriptorHeapType;
			if constexpr (type == CBV_SRV_UAV)return m_SRVHeap.get();
			if constexpr (type == DSV)return m_DSVHeap.get();
			if constexpr (type == RTV)return m_RTVHeap.get();
			if constexpr (type == SAMPLER)return m_SamplerHeap.get();
			return nullptr;
		}

		void BeginUpload(CommandList* cmd);
		void Upload(Resource* dst, Subresource* data, uint count);
		void Upload(Resource* dst, void* data, size_t sizeInBytes);
		void CommitUpload();

		inline auto GetAllocator() { return m_Allocator.Get(); }
		inline operator ID3D12Device* () { return m_Device.Get(); }

	private:
		ComPtr<IDXGIAdapter1> m_Adapter;
		ComPtr<ID3D12Device5> m_Device;
		ComPtr<IDXGIFactory6> m_Factory;
		ComPtr<D3D12MA::Allocator> m_Allocator;		
		DeferredSyncedReleaseQueue<Descriptor> m_DeferredDeleteDescriptors;

		struct UploadContext {
			CommandList* cmd;
			std::vector<std::unique_ptr<Resource>> intermediates;
			SyncFence uploadFence;

			void Open(CommandList* _cmd) { cmd = _cmd; Clean(); }
			bool IsOpen() { return cmd != nullptr; }
			void RecordIntermediate(std::unique_ptr<Resource>&& intermediate) { intermediates.push_back(intermediate); }
			void UploadAndClose() { uploadFence = cmd->Execute(); cmd = nullptr; }
			void Clean() { if (uploadFence.IsCompleted()) intermediates.clear(); }
		} m_UploadContext;
		
		std::unique_ptr<CommandQueue> 
			m_DirectQueue, 
			m_CopyQueue, 
			m_ComputeQueue;
		std::unique_ptr<DescriptorHeap>
			m_RTVHeap,
			m_DSVHeap,
			m_SRVHeap,
			m_SamplerHeap;

	};
	
}