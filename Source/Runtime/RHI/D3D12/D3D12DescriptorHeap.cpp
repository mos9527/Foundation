#include "D3D12DescriptorHeap.hpp"
#include "D3D12Device.hpp"
#include "D3D12Resource.hpp"
namespace RHI {    
    /* Heap */
    DescriptorHeap::DescriptorHeap(Device* device, DescriptorHeapDesc const& cfg) : DeviceChild(device), m_Config(cfg), m_FreeList(cfg.descriptorCount) {
        CHECK(cfg.descriptorCount <= 2048 || !cfg.shaderVisible);
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = m_Config.descriptorCount;
        rtvHeapDesc.Type = DescriptorHeapTypeToD3DType(m_Config.heapType);
        rtvHeapDesc.Flags = m_Config.shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        CHECK_HR(device->GetNativeDevice()->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_DescriptorHeap)));
        m_HeadHandle.heap_handle = 0;
        m_HeadHandle.cpu_handle = m_DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
        if (m_Config.shaderVisible)
            m_HeadHandle.gpu_handle = m_DescriptorHeap->GetGPUDescriptorHandleForHeapStart();        
        m_HeapIncrementSize = device->GetNativeDevice()->GetDescriptorHandleIncrementSize(DescriptorHeapTypeToD3DType(m_Config.heapType));
    }
    Descriptor DescriptorHeap::GetDescriptor(uint handle) {
        auto new_handle = m_HeadHandle;
        new_handle.Increment(handle, m_HeapIncrementSize);
        return new_handle;
    }
    Descriptor DescriptorHeap::AllocateDescriptor() {       
        std::scoped_lock lock(alloc_mutex);        
        auto heap_handle = m_FreeList.pop();
        return GetDescriptor(heap_handle);
    };
    void DescriptorHeap::FreeDescriptor(uint handle) {
        std::scoped_lock lock(alloc_mutex);
        m_FreeList.push(handle);
    }
    void DescriptorHeap::FreeDescriptor(Descriptor desc) {
        if (desc.is_valid())
            FreeDescriptor(desc.heap_handle);
    }

    OnlineDescriptorHeap::OnlineDescriptorHeap(Device* device, DescriptorHeap::DescriptorHeapDesc const& cfg, uint pageSize, uint persistentCount) :
        DeviceChild(device), m_Heap(device, cfg) 
    {
        const uint transientCount = cfg.descriptorCount - persistentCount;
        CHECK(transientCount% pageSize == 0 && "descriptorCount - presistentCount must be multiple of pageSize");
        const uint numPages = transientCount / pageSize;
        for (uint i = 0; i < numPages; i++) {
            m_Pages.push_back({ i * pageSize, (i + 1) * pageSize,0 });
        }
        m_FreePages.reset(numPages);
        m_FilledPages.reserve(numPages);
        m_CurrentPage.resize(+CommandListType::NUM_TYPES); // Each type of command list (i.e. each GPU queue) has its own page
        // In Foundation there are only 3 GPU queues (Direct,Compute,and Copy)
        // Doing so allows us to avoid race conditions where one queue flags heaps of other queues to be freed
        for (auto& page : m_CurrentPage) 
            page = AllocatePage();
        m_PersistentFreeDescriptors.reset(persistentCount, transientCount); // persistent descriptors lays after the transitent ones
        LOG(INFO) << "Created online heap. Pages=" << numPages << " PageSize=" << pageSize << " PesistentSize=" << persistentCount;
    }

    void OnlineDescriptorHeap::TryCleanPages() {
        for (auto it = m_PagesToFree.begin();it != m_PagesToFree.end();it++){                        
            if (it->first.IsCompleted()) {
                m_Pages[it->second].Reset();
                m_FreePages.push(it->second);
                it = m_PagesToFree.erase(it);
                if (it == m_PagesToFree.end())
                    break;
            }
        }
    }

    uint OnlineDescriptorHeap::AllocatePage() {
        if (!m_FreePages.size()) TryCleanPages();
        CHECK(m_FreePages.size() && "No more free page! Did you call SetCleanupSyncPoint(sync)?");
        const uint pageIndex = m_FreePages.pop();
        return pageIndex;
    }

    Descriptor OnlineDescriptorHeap::AllocatePersistentDescriptor() {
        std::scoped_lock lock(alloc_mutex);
        uint handle = m_PersistentFreeDescriptors.pop();
        return m_Heap.GetDescriptor(handle);
    }

    void OnlineDescriptorHeap::FreePersistentDescriptor(Descriptor& descriptor) {
        std::scoped_lock lock(alloc_mutex);
        if (descriptor.is_valid()) {
            m_PersistentFreeDescriptors.push(descriptor.get_heap_handle());
            descriptor.invalidate();
        }
    }
   
    Descriptor OnlineDescriptorHeap::AllocateTransitentDescriptor(CommandListType ctxType) {
        std::scoped_lock lock(alloc_mutex);
        if (m_Pages[m_CurrentPage[+ctxType]].IsFull()) {
            m_FilledPages.push_back(m_CurrentPage[+ctxType]);
            m_CurrentPage[+ctxType] = AllocatePage();
        }
        uint handle = m_Pages[m_CurrentPage[+ctxType]].AllocateDescriptor();
        return m_Heap.GetDescriptor(handle);
    }

    void OnlineDescriptorHeap::SetCleanupSyncPoint(SyncFence const& sync) {
        std::scoped_lock lock(alloc_mutex);
        for (auto& page : m_FilledPages)
            m_PagesToFree.push_back({ sync, page });
        m_FilledPages.clear();
    }
}