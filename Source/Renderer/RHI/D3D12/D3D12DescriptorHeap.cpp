#include "D3D12DescriptorHeap.hpp"
#include "D3D12Device.hpp"
namespace RHI {
    static D3D12_DESCRIPTOR_HEAP_TYPE GetDescriptorHeapD3DType(DescriptorHeap::HeapType type) {
        switch (type)
        {
        case DescriptorHeap::CBV_SRV_UAV:
            return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        case DescriptorHeap::SAMPLER:
            return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        case DescriptorHeap::RTV:
            return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        case DescriptorHeap::DSV:
            return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        default:
            return (D3D12_DESCRIPTOR_HEAP_TYPE)-1;
        }
    }
    /* Heap Handle */
    Descriptor::~Descriptor() {
        if (IsValid())
            heap_ref.Free(heap_handle);
    }

    /* Heap */
    DescriptorHeap::DescriptorHeap(Device* device, DescriptorHeapDesc const& cfg) : m_Config(cfg) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = m_Config.descriptorCount;
        rtvHeapDesc.Type = GetDescriptorHeapD3DType(m_Config.heapType);
        rtvHeapDesc.Flags = m_Config.shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        CHECK_HR(device->GetNativeDevice()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_DescriptorHeap)));
        m_HeadHandle.heap_handle = 0;
        m_HeadHandle.cpu_handle = m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        if (m_Config.shaderVisible) m_HeadHandle.gpu_handle = m_DescriptorHeap->GetGPUDescriptorHandleForHeapStart();        
        m_HeapIncrementSize = device->GetNativeDevice()->GetDescriptorHandleIncrementSize(GetDescriptorHeapD3DType(m_Config.heapType));
        m_IndexQueue.setup(cfg.descriptorCount);
    }

    std::shared_ptr<Descriptor> DescriptorHeap::GetDescriptor() {
        auto new_handle = std::make_shared<Descriptor>(m_HeadHandle);
        auto heap_handle = m_IndexQueue.pop();
        new_handle->Increment(heap_handle, m_HeapIncrementSize);
        return new_handle;
    };
}