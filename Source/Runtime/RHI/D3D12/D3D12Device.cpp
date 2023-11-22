#include "D3D12Device.hpp"
#include "D3D12Resource.hpp"
#include "D3D12CommandList.hpp"
#ifdef RHI_D3D12_USE_AGILITY
extern "C" { __declspec(dllexport) extern const uint D3D12SDKVersion = 4; }
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
            for (uint groupIndex = 0; groupIndex < 2; ++groupIndex)
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
        if (allocator) {
            D3D12MA::Budget budget;
            allocator->GetBudget(&budget, NULL);        
            LOG(INFO) << "[ Allocator ]";
            LOG(INFO) << "  Allocations: " << budget.Stats.AllocationCount;
            LOG(INFO) << "  Allocation size: " << size_to_str(budget.Stats.AllocationBytes);
            LOG(INFO) << "  Blocks: " << budget.Stats.BlockCount;
            LOG(INFO) << "  Block total size: " << size_to_str(budget.Stats.BlockBytes);
            LOG(INFO) << "  D3D12 Usage: " << size_to_str(budget.UsageBytes);
            LOG(INFO) << "  D3D12 Budget: " << size_to_str(budget.BudgetBytes);
        }
    }
    void LogD3D12MAPoolStatistics(D3D12MA::Pool* pool) {
        if (pool) {
            D3D12MA::Statistics stats;
            pool->GetStatistics(&stats);
            LOG(INFO) << "[ Pool '" << wstring_to_utf8(pool->GetName()).c_str() << "' ]";
            LOG(INFO) << "  Allocations: " << stats.AllocationCount;
            LOG(INFO) << "  Allocation size: " << size_to_str(stats.AllocationBytes);
            LOG(INFO) << "  Blocks: " << stats.BlockCount;
            LOG(INFO) << "  Block total size: " << size_to_str(stats.BlockBytes);
        }
    }
#pragma endregion
    static void CheckDeviceCapabilities(ID3D12Device* device) {
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = { D3D_SHADER_MODEL_6_6 };
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel)))
            || (shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_6))
        {
            LOG(FATAL) << "ERROR: Shader Model 6.6 is not supported on this device";            
        }
#ifdef RHI_USE_MESH_SHADER
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 features = {};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &features, sizeof(features)))
            || (features.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED))
        {
            LOG(FATAL) << "ERROR: Mesh Shaders aren't supported on this device";
        }
#endif
    }
    static ComPtr<IDXGIAdapter1> GetHardwareAdapter(IDXGIFactory1* pFactory, uint adapterIndex = 0) {
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIFactory6> factory6;
        CHECK_HR(pFactory->QueryInterface(IID_PPV_ARGS(&factory6)));
        for (
            uint index = 0, count = 0;
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
    void CHECK_DEVICE_REMOVAL(Device* device, HRESULT hr) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED) {
            auto pDevice = device->GetNativeDevice();
            ComPtr<ID3D12DeviceRemovedExtendedData1> pDred;
            if SUCCEEDED((pDevice->QueryInterface(IID_PPV_ARGS(&pDred)))) {
                D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 DredAutoBreadcrumbsOutput;
                D3D12_DRED_PAGE_FAULT_OUTPUT DredPageFaultOutput;
                CHECK_HR(pDred->GetAutoBreadcrumbsOutput1(&DredAutoBreadcrumbsOutput));
                CHECK_HR(pDred->GetPageFaultAllocationOutput(&DredPageFaultOutput));
#ifdef _DEBUG
                __debugbreak();
#endif            
            }
            LOG(ERROR) << "Device removal (TDR) has been detected. Reason:";
            LOG_SYSRESULT(pDevice->GetDeviceRemovedReason());            
            return;
        } 
        CHECK_HR(hr);
    }
    void Device::UploadContext::RecordIntermediate(std::unique_ptr<Resource>&& intermediate) { 
        intermediates.push_back(std::move(intermediate));
    }
    SyncFence Device::UploadContext::UploadAndClose() { 
        CHECK(cmd->IsOpen());
        cmd->End();
        uploadFence = cmd->Execute(); 
        cmd = nullptr; 
        return uploadFence; 
    }
    bool Device::UploadContext::Clean() { 
        if (uploadFence.IsCompleted())
        {
            intermediates.clear();
            return true;
        }
        return false;
    }
    Device::Device(DeviceDesc cfg) {
#ifdef _DEBUG    
        ComPtr<ID3D12Debug> debugInterface;
        CHECK_HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
        debugInterface->EnableDebugLayer();
        CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&m_Factory));

        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> pDredSettings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings)))) {
            // Turn on AutoBreadcrumbs and Page Fault reporting
            pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
        else {
            LOG(WARNING) << "D3D12 DRED Unavailable!";
            // xxx feature support masks
        }
#else
        CreateDXGIFactory2(NULL, IID_PPV_ARGS(&dxgiFactory));
#endif
        m_Adapter = GetHardwareAdapter(m_Factory.Get(), cfg.AdapterIndex);
        CHECK_HR(D3D12CreateDevice(m_Adapter.Get(), D3D_MIN_FEATURE_LEVEL, IID_PPV_ARGS(&m_Device)));
        CheckDeviceCapabilities(m_Device.Get());
        LOG(INFO) << "D3D12 Device created";
        LogDeviceInformation(m_Device.Get());
        // Command queues
        m_DirectQueue = std::make_unique<CommandQueue>(this, CommandListType::Direct);
        m_DirectQueue->SetName(L"Direct Command Queue");
        m_CopyQueue = std::make_unique<CommandQueue>(this, CommandListType::Copy);
        m_CopyQueue->SetName(L"Copy Command Queue");
        m_ComputeQueue = std::make_unique<CommandQueue>(this, CommandListType::Compute);
        m_ComputeQueue->SetName(L"Compute Command Queue");
        // Command lists
        m_DirectCmd = std::make_unique<CommandList>(this, CommandListType::Direct, RHI_DEFAULT_DIRECT_COMMAND_ALLOCATOR_COUNT);
        m_DirectCmd->SetName(L"3D Command List");
        m_CopyCmd = std::make_unique<CommandList>(this, CommandListType::Copy, RHI_DEFAULT_COPY_COMMAND_ALLOCATOR_COUNT);
        m_CopyCmd->SetName(L"Copy Command List");
        m_ComputeCmd = std::make_unique<CommandList>(this, CommandListType::Compute, RHI_DEFAULT_COMPUTE_COMMAND_ALLOCATOR_COUNT);
        m_ComputeCmd->SetName(L"Compute Command List");
        // Descriptor heaps
        m_RTVHeap = std::make_unique<DescriptorHeap>(this, DescriptorHeap::DescriptorHeapDesc{
            .shaderVisible = false,
                .descriptorCount = RHI_ALLOC_SIZE_DESCHEAP,
                .heapType = DescriptorHeapType::RTV
        });
        m_RTVHeap->SetName(L"RTV Heap");
        m_DSVHeap = std::make_unique<DescriptorHeap>(this, DescriptorHeap::DescriptorHeapDesc{
            .shaderVisible = false,
                .descriptorCount = RHI_ALLOC_SIZE_DESCHEAP,
                .heapType = DescriptorHeapType::DSV
        });
        m_DSVHeap->SetName(L"DSV Heap");
        m_SRVHeap = std::make_unique<DescriptorHeap>(this, DescriptorHeap::DescriptorHeapDesc{
            .shaderVisible = false,
                .descriptorCount = RHI_ALLOC_SIZE_DESCHEAP,
                .heapType = DescriptorHeapType::CBV_SRV_UAV
        });
        m_SRVHeap->SetName(L"OFFLINE CBV_SRV_UAV Heap");
        m_SamplerHeap = std::make_unique<DescriptorHeap>(this, DescriptorHeap::DescriptorHeapDesc{
            .shaderVisible = false,
                .descriptorCount = RHI_ALLOC_SIZE_DESCHEAP,
                .heapType = DescriptorHeapType::SAMPLER
        });
        m_SamplerHeap->SetName(L"OFFLINE Sampler Heap");
        m_OnlineSRVHeap = std::make_unique<DescriptorHeap>(this, DescriptorHeap::DescriptorHeapDesc{
            .shaderVisible = true,
                .descriptorCount = RHI_ALLOC_SIZE_SHADER_VISIBLE_DESCHEAP,
                .heapType = DescriptorHeapType::CBV_SRV_UAV
        });
        m_OnlineSRVHeap->SetName(L"CBV_SRV_UAV Heap");
        m_OnlineSamplerHeap = std::make_unique<DescriptorHeap>(this, DescriptorHeap::DescriptorHeapDesc{
            .shaderVisible = true,
                .descriptorCount = RHI_ALLOC_SIZE_SHADER_VISIBLE_DESCHEAP,
                .heapType = DescriptorHeapType::SAMPLER
        });
        m_OnlineSamplerHeap->SetName(L"Sampler Heap");
        // D3D12MA
        D3D12MA::ALLOCATOR_DESC allocatorDesc{};
        allocatorDesc.pDevice = m_Device.Get();
        allocatorDesc.pAdapter = m_Adapter.Get();
        allocatorDesc.PreferredBlockSize = 0;
        D3D12MA::CreateAllocator(&allocatorDesc, &m_Allocator);
        /* Utilities */
        // Zero(reset) buffer
        m_ZeroBuffer = std::make_unique<Buffer>(this, Resource::ResourceDesc::GetGenericBufferDesc(sizeof(uint) * RHI_ZERO_BUFFER_SIZE, sizeof(uint)));
        m_ZeroBuffer->SetName(L"Zero buffer");
        std::vector<uint> zeros(RHI_ZERO_BUFFER_SIZE);
        m_ZeroBuffer->Update(zeros.data(), sizeof(uint) * RHI_ZERO_BUFFER_SIZE, 0);
    }
    Device::~Device() {
        m_Factory->Release();        
    }    

    void Device::Wait() {
        m_DirectQueue->Wait();
        m_CopyQueue->Wait();
        m_ComputeQueue->Wait();
    }
}