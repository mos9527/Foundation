#include "D3D12DescriptorHeap.hpp"
#include "D3D12Device.hpp"
#include "D3D12Resource.hpp"
namespace RHI {
    void Descriptor::release() { 
        if (owner) 
            owner->FreeDescriptor(heap_handle);
        heap_handle = INVALID_HEAP_HANDLE;
    }
    /* Heap */
    DescriptorHeap::DescriptorHeap(Device* device, DescriptorHeapDesc const& cfg) : DeviceChild(device), m_Config(cfg) {
        CHECK(cfg.descriptorCount <= 2048 || !cfg.shaderVisible);
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = m_Config.descriptorCount;
        rtvHeapDesc.Type = DescriptorHeapTypeToD3DType(m_Config.heapType);
        rtvHeapDesc.Flags = m_Config.shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        CHECK_HR(device->GetNativeDevice()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_DescriptorHeap)));
        m_HeadHandle.heap_handle = 0;
        m_HeadHandle.cpu_handle = m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        if (m_Config.shaderVisible) m_HeadHandle.gpu_handle = m_DescriptorHeap->GetGPUDescriptorHandleForHeapStart();        
        m_HeapIncrementSize = device->GetNativeDevice()->GetDescriptorHandleIncrementSize(DescriptorHeapTypeToD3DType(m_Config.heapType));
        m_IndexQueue.setup(cfg.descriptorCount);
    }
    Descriptor DescriptorHeap::GetDescriptor(uint handle) {
        auto new_handle = m_HeadHandle;
        new_handle.Increment(handle, m_HeapIncrementSize);
        return new_handle;
    }
    Descriptor DescriptorHeap::AllocateDescriptor() {        
        auto heap_handle = m_IndexQueue.pop();        
        return GetDescriptor(heap_handle);
    };
    void DescriptorHeap::FreeDescriptor(uint handle) { 
        m_IndexQueue.push(handle); 
    };

    void DescriptorHeap::FreeDescriptor(Descriptor desc) {
        if (desc.is_valid())
            FreeDescriptor(desc.heap_handle);
    };

    void DescriptorHeap::CopyInto(Descriptor descriptorSrc, Descriptor descriptorDst) {
        m_Device->GetNativeDevice()->CopyDescriptorsSimple(
            1, descriptorDst.cpu_handle, descriptorSrc.cpu_handle, DescriptorHeapTypeToD3DType(m_Config.heapType)
        );
    }

}