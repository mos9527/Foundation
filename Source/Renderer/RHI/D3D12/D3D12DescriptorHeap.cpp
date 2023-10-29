#include "D3D12DescriptorHeap.hpp"
#include "D3D12Device.hpp"
namespace RHI {
    /* Heap */
    DescriptorHeap::DescriptorHeap(Device* device, DescriptorHeapDesc const& cfg) : m_Config(cfg), pDevice(device) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = m_Config.descriptorCount;
        rtvHeapDesc.Type = (D3D12_DESCRIPTOR_HEAP_TYPE)m_Config.heapType;
        rtvHeapDesc.Flags = m_Config.shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        CHECK_HR(device->GetNativeDevice()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_DescriptorHeap)));
        m_HeadHandle.heap_handle = 0;
        m_HeadHandle.cpu_handle = m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        if (m_Config.shaderVisible) m_HeadHandle.gpu_handle = m_DescriptorHeap->GetGPUDescriptorHandleForHeapStart();        
        m_HeapIncrementSize = device->GetNativeDevice()->GetDescriptorHandleIncrementSize((D3D12_DESCRIPTOR_HEAP_TYPE)m_Config.heapType);
        m_HandleQueue.Setup(cfg.descriptorCount);
    }

    DescriptorHandle DescriptorHeap::GetDescriptor(handle_type heap_handle) {
        DescriptorHandle new_handle = m_HeadHandle;
        new_handle.Increment(heap_handle, m_HeapIncrementSize);
        return new_handle;
    };
}