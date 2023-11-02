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
#define ALLOC_SIZE_DESC_HEAP 1024
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

		DescriptorHandle GetRenderTargetView(Texture* tex);
		DescriptorHandle GetBufferShaderResourceView(Buffer* buf, ResourceFormat format = ResourceFormat::Unknown);
		DescriptorHandle GetTexture2DShaderResourceView(Buffer* buf, ResourceDimensionSRV view);
		DescriptorHandle GetConstantBufferView(Buffer* buf);

		std::shared_ptr<Buffer> AllocateIntermediateBuffer(Buffer::BufferDesc const& desc);
		void FlushIntermediateBuffers();

		inline auto GetDXGIFactory() { return m_Factory; }
		inline auto GetNativeDevice() { return m_Device; }
		
		inline auto GetCommandQueue(CommandListType type) { return m_CommandQueues[VALUE_OF(type)].get(); }
		inline auto GetDescriptorHeap(DescriptorHeap::HeapType type) { return m_DescriptorHeaps[type].get(); }
		inline auto GetRootSignature() { return m_RootSignature.Get(); }
		inline auto GetCommandSignature(CommandSignature::IndirectArgumentType type) { return m_CommandSignatures[type].get(); }
		inline auto GetAllocator() { return m_Allocator.Get(); }
		inline auto GetAllocatorPool(ResourcePoolType type) { return m_AllocatorPools[VALUE_OF(type)].Get(); }
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
		ComPtr<ID3D12RootSignature> m_RootSignature;

		std::vector<ComPtr<D3D12MA::Pool>> m_AllocatorPools;
		std::vector<std::unique_ptr<CommandQueue>> m_CommandQueues;
		std::vector<std::unique_ptr<DescriptorHeap>> m_DescriptorHeaps;		
		std::vector<std::shared_ptr<Buffer>> m_IntermediateBuffers;
		std::vector<std::unique_ptr<CommandSignature>> m_CommandSignatures;
	};
}