#pragma once
#include "../Common.hpp"
#include "D3D12Swapchain.hpp"
#include "D3D12CommandQueue.hpp"
#include "D3D12DescriptorHeap.hpp"
#include "D3D12Texture.hpp"
#include "D3D12Types.hpp"
#include "D3D12CommandList.hpp"
#include "D3D12Fence.hpp"
#include "D3D12CommandSignature.hpp"

namespace RHI {
	void LogDeviceInformation(ID3D12Device* device);
	void LogAdapterInformation(IDXGIAdapter1* adapter);
	void LogD3D12MABudget(D3D12MA::Allocator* allocator);
	void LogD3D12MAPoolStatistics(D3D12MA::Pool* pool);
	class Device : public RHIObject {
	public:
		struct DeviceDesc {
			uint AdapterIndex{ 0 };
		};
		Device(DeviceDesc cfg);
		Device(Device&) = delete;
		Device(const Device&&) = delete;
		~Device();

		std::shared_ptr<Buffer> AllocateIntermediateBuffer(Buffer::BufferDesc const& desc);
		void FlushIntermediateBuffers();

		inline auto GetDXGIFactory() { return m_Factory; }
		inline auto GetNativeDevice() { return m_Device; } // TODO : Reduce the usage of GetNative*()
		
		inline auto GetCommandQueue(CommandListType type) { return m_CommandQueues[+type].get(); }	
		inline auto GetCommandSignature(IndirectArgumentType type) { return m_CommandSignatures[+type].get(); }
		inline auto GetAllocator() { return m_Allocator.Get(); }
		inline auto GetAllocatorPool(ResourcePoolType type) { return m_AllocatorPools[+type].Get(); }
		inline operator ID3D12Device* () { return m_Device.Get(); }

		using RHIObject::GetName;
		inline void SetName(name_t name) {
			m_Name = name;
			m_Device->SetName((const wchar_t*)name.c_str());
		}
	private:
		using RHIObject::m_Name;
		ComPtr<IDXGIAdapter1> m_Adapter;
		ComPtr<ID3D12Device5> m_Device;
		ComPtr<IDXGIFactory6> m_Factory;
		ComPtr<D3D12MA::Allocator> m_Allocator;		

		std::vector<ComPtr<D3D12MA::Pool>> m_AllocatorPools;
		std::vector<std::unique_ptr<CommandQueue>> m_CommandQueues;
		std::vector<std::shared_ptr<Buffer>> m_IntermediateBuffers;
		std::vector<std::unique_ptr<CommandSignature>> m_CommandSignatures;
	};
}