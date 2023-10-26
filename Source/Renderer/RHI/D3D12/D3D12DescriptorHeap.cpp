#include "D3D12DescriptorHeap.hpp"
#include "D3D12Descriptor.hpp"
#include "D3D12Device.hpp"
namespace RHI {
    /* Heap */
    Descriptor DescriptorHeap::GetByHandle(handle_type handle) {
        CHECK(handle < m_Config.DescriptorCount);
        return m_HeadHandle + Descriptor::Offset(handle, m_HeapIncrementSize);
    };
    DescriptorHeap::DescriptorHeap(Device* device, DescriptorHeapConfig const& cfg) : m_Config(cfg) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = m_Config.DescriptorCount;
        rtvHeapDesc.Type = (D3D12_DESCRIPTOR_HEAP_TYPE)m_Config.Type;
        rtvHeapDesc.Flags = m_Config.ShaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        CHECK_HR(device->GetNativeDevice()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_DescriptorHeap)));
        m_HeadHandle.cpu_handle = m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        if (m_Config.ShaderVisible) m_HeadHandle.gpu_handle = m_DescriptorHeap->GetGPUDescriptorHandleForHeapStart();
        m_HeadHandle.handle = 0;
        m_HeapIncrementSize = device->GetNativeDevice()->GetDescriptorHandleIncrementSize((D3D12_DESCRIPTOR_HEAP_TYPE)m_Config.Type);
    }
    /* Allocator */
    DescriptorHeapAllocator::DescriptorHeapAllocator(Device* device, DescriptorHeapConfig const& cfg) : DescriptorHeap(device, cfg) {
        m_DescriptorStates.resize(cfg.DescriptorCount);
    }
    Descriptor DescriptorHeapAllocator::Allocate() {
        for (UINT i = 0; i < m_Config.DescriptorCount; i++) {
            if (!m_DescriptorStates[i]) {
                m_DescriptorStates[i] = true;
                return GetByHandle(i);
            }
        }
        return {};
    }
    void DescriptorHeapAllocator::Free(handle_t handle) {   
        LOG(INFO) << "FREE " << handle;
        CHECK(handle < m_DescriptorStates.size());
        m_DescriptorStates[handle] = false;
    }
}