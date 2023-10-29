#include "D3D12Device.hpp"
#include "../../Helpers.hpp"
#ifdef RHI_D3D12_USE_AGILITY
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 4; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }
#endif
namespace RHI {
#pragma region Logging Helpers
    // https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator/blob/master/src/D3D12Sample.cpp
    void LogDeviceInformation(ID3D12Device* device) {
        D3D12_FEATURE_DATA_ARCHITECTURE1 architecture1 = {};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &architecture1, sizeof architecture1)))
        {
            LOG(INFO) << "Device Features:";
            LOG(INFO) << "  UMA: " << architecture1.UMA;
            LOG(INFO) << "  CacheCoherentUMA: " << architecture1.CacheCoherentUMA;
            LOG(INFO) << "  IsolatedMMU: " << architecture1.IsolatedMMU;
        }
    }
    void LogAdapterInformation(IDXGIAdapter1* adapter)
    {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))))
        {        
            for (UINT groupIndex = 0; groupIndex < 2; ++groupIndex)
            {
                const DXGI_MEMORY_SEGMENT_GROUP group = groupIndex == 0 ? DXGI_MEMORY_SEGMENT_GROUP_LOCAL : DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL;
                const char* const groupName = groupIndex == 0 ? "DXGI_MEMORY_SEGMENT_GROUP_LOCAL" : "DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL";
                DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
                CHECK_HR(adapter3->QueryVideoMemoryInfo(0, group, &info));
                LOG(INFO) << groupName;
                LOG(INFO) << "  Budget = " << size_to_str(info.Budget);
                LOG(INFO) << "  CurrentUsage = " << size_to_str(info.CurrentUsage);
                LOG(INFO) << "  AvailableForReservation = " << size_to_str(info.AvailableForReservation);
                LOG(INFO) << "  CurrentReservation = " << size_to_str(info.CurrentReservation);            
            }
        }
    }

    void LogD3D12MABudget(D3D12MA::Allocator* allocator) {
        D3D12MA::Budget budget;
        allocator->GetBudget(&budget, NULL);
        LOG(INFO) << "Allocator Statisitics";
        LOG(INFO) << "  Allocations: " << budget.Stats.AllocationCount;
        LOG(INFO) << "  Allocation size: " << size_to_str(budget.Stats.AllocationBytes);
        LOG(INFO) << "  Blocks: " << budget.Stats.BlockCount;
        LOG(INFO) << "  Block total size: " << size_to_str(budget.Stats.BlockBytes);
        LOG(INFO) << "  D3D12 Usage: " << size_to_str(budget.UsageBytes);
        LOG(INFO) << "  D3D12 Budget: " << size_to_str(budget.BudgetBytes);
    }
#pragma endregion

    static ComPtr<IDXGIAdapter1> GetHardwareAdapter(IDXGIFactory1* pFactory, UINT adapterIndex = 0) {
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIFactory6> factory6;
        CHECK_HR(pFactory->QueryInterface(IID_PPV_ARGS(&factory6)));
        for (
            UINT index = 0, count = 0;
            SUCCEEDED(factory6->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter)));
            index++
        ) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            // Check if we can use the adapter to create a D3D12 device
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_MIN_FEATURE_LEVEL, _uuidof(ID3D12Device), nullptr))) {
                DLOG(INFO) << "Found hardware D3D12 Adapter " << wstring_to_utf8(desc.Description) <<  " [" << count << "]";
                LogAdapterInformation(adapter.Get());
                if (count >= adapterIndex) return adapter;
                count++;
            }

        }
        LOG(FATAL) << "No available D3D12 hardware found!";    
        return {};
    }

    DescriptorHandle Device::CreateRenderTargetView(Texture* tex) {
        auto handle = GetCpuAllocator(DescriptorHeap::HeapType::RTV)->Allocate();
        m_Device->CreateRenderTargetView(*tex, nullptr, handle.cpu_handle);
        return handle;
    }
    DescriptorHandle Device::GetShaderResourceView(Buffer* buf, ResourceDimensionSRV view) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = (DXGI_FORMAT)buf->GetDesc().format;
        srvDesc.ViewDimension = (D3D12_SRV_DIMENSION)view;
        srvDesc.Texture2D.MipLevels = buf->GetDesc().mipLevels;
        auto handle = GetCpuAllocator(DescriptorHeap::HeapType::CBV_SRV_UAV)->Allocate();
        m_Device->CreateShaderResourceView(*buf, &srvDesc, handle.cpu_handle);
        return handle;
    }
    DescriptorHandle Device::GetConstantBufferView(Buffer* buf) {
        CHECK(buf->GetDesc().dimension == ResourceDimension::Buffer);
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = buf->GetGPUAddress();
        cbvDesc.SizeInBytes = buf->GetDesc().width;
        auto handle = GetCpuAllocator(DescriptorHeap::HeapType::CBV_SRV_UAV)->Allocate();
        m_Device->CreateConstantBufferView(&cbvDesc, handle.cpu_handle);
        return handle;
    }
    void Device::CreateSwapchainAndBackbuffers(Swapchain::SwapchainDesc const& cfg) {
        m_SwapChain = std::make_unique<Swapchain>(this, cfg);
        m_SwapChain->Resize(this, cfg.InitWidth, cfg.InitHeight);
        m_CommandLists[CommandList::CommandListType::DIRECT] = std::make_unique<CommandList>(
            this,
            CommandList::CommandListType::DIRECT,
            cfg.BackBufferCount
        );
    }
    Device::Device(DeviceDesc cfg) {
#ifdef _DEBUG    
        ComPtr<ID3D12Debug> debugInterface;
        CHECK_HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
        debugInterface->EnableDebugLayer();
#endif

#ifdef _DEBUG
        CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&m_Factory));
#else
        CreateDXGIFactory2(NULL, IID_PPV_ARGS(&dxgiFactory));
#endif
        m_Adapter = GetHardwareAdapter(m_Factory.Get(), cfg.AdapterIndex);
        CHECK_HR(D3D12CreateDevice(m_Adapter.Get(), D3D_MIN_FEATURE_LEVEL, IID_PPV_ARGS(&m_Device)));
        LOG(INFO) << "D3D12 Device created";
        LogDeviceInformation(m_Device.Get());
        m_CommandQueue = std::make_unique<CommandQueue>(this);
        m_DescriptorHeaps.resize(DescriptorHeap::HeapType::NUM_TYPES);
        m_DescriptorHeaps[DescriptorHeap::HeapType::RTV] = std::make_unique<DescriptorHeap>(
            this,
            DescriptorHeap::DescriptorHeapDesc{
            .shaderVisible = false,
                .descriptorCount = ALLOC_SIZE_DESC_HEAP,
                .heapType = DescriptorHeap::HeapType::RTV
        });
        m_DescriptorHeaps[DescriptorHeap::HeapType::CBV_SRV_UAV] = std::make_unique<DescriptorHeap>(
            this,
            DescriptorHeap::DescriptorHeapDesc{
            .shaderVisible = true,
            .descriptorCount = ALLOC_SIZE_DESC_HEAP,
            .heapType = DescriptorHeap::HeapType::CBV_SRV_UAV
        });
        m_CommandLists.resize(CommandList::CommandListType::NUM_TYPES);
        m_MarkerFence = std::make_unique<MarkerFence>(this);
        D3D12MA::ALLOCATOR_DESC allocatorDesc{};
        allocatorDesc.pDevice = m_Device.Get();
        allocatorDesc.pAdapter = m_Adapter.Get();
        allocatorDesc.PreferredBlockSize = 0;
        D3D12MA::CreateAllocator(&allocatorDesc, &m_Allocator);
    }
    void Device::BeginFrame() {
        auto directList = GetCommandList(CommandList::DIRECT);
        directList->ResetAllocator(m_SwapChain->nBackbufferIndex);
        directList->Begin(m_SwapChain->nBackbufferIndex);
    }
    void Device::EndFrame(bool vsync) {
        auto list = GetCommandList(CommandList::DIRECT);
        list->End();
        m_CommandQueue->Execute(list);
        m_SwapChain->PresentAndMoveToNextFrame(m_CommandQueue.get(), vsync);
    }
    void Device::WaitForGPU() {
        m_MarkerFence->MarkAndWait(m_CommandQueue.get());
    }
    std::shared_ptr<Buffer> Device::AllocateIntermediateBuffer(Buffer::BufferDesc desc) {
        auto buffer = std::make_shared<Buffer>(this, desc);
        m_IntermediateBuffers.push_back(buffer);
        buffer->SetName(std::format(L"Intermediate Buffer #{}", m_IntermediateBuffers.size()));
        return m_IntermediateBuffers.back();
    }
    void Device::FlushIntermediateBuffers() {
        m_IntermediateBuffers.clear();
    }
    Device::~Device() {
        m_Factory->Release();        
    }
}