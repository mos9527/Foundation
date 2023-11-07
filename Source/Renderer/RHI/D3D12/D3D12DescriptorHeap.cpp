#include "D3D12DescriptorHeap.hpp"
#include "D3D12Device.hpp"
namespace RHI {
    /* Heap Handle */
    Descriptor::~Descriptor() {
        if (IsValid())
            heap_ref.Free(heap_handle);
    }

    /* Heap */
    DescriptorHeap::DescriptorHeap(Device* device, DescriptorHeapDesc const& cfg) : m_Config(cfg) {
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

    std::unique_ptr<Descriptor> DescriptorHeap::AllocateDescriptor() {
        auto new_handle = std::make_unique<Descriptor>(m_HeadHandle);
        auto heap_handle = m_IndexQueue.pop();
        new_handle->Increment(heap_handle, m_HeapIncrementSize);
        return new_handle;
    };

    void DescriptorHeap::CopyInto(Device* device, Descriptor* descriptorSrc, Descriptor* descriptorDst) {
        CHECK(descriptorSrc->heap_ref.m_Config.heapType == descriptorDst->heap_ref.m_Config.heapType);
        CHECK(descriptorSrc->heap_ref.m_Config.shaderVisible == false);
        device->GetNativeDevice()->CopyDescriptorsSimple(
            1, descriptorDst->cpu_handle, descriptorSrc->cpu_handle, DescriptorHeapTypeToD3DType(descriptorSrc->heap_ref.m_Config.heapType)
        );
    }
}