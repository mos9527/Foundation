#include "D3D12Device.hpp"

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
#ifdef RHI_USE_D3D12_MESH_SHADER
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
#ifdef _DEBUG
            ComPtr<ID3D12DeviceRemovedExtendedData1> pDred;
            CHECK_HR(pDevice->QueryInterface(IID_PPV_ARGS(&pDred)));

            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 DredAutoBreadcrumbsOutput;
            D3D12_DRED_PAGE_FAULT_OUTPUT DredPageFaultOutput;
            CHECK_HR(pDred->GetAutoBreadcrumbsOutput1(&DredAutoBreadcrumbsOutput));
            CHECK_HR(pDred->GetPageFaultAllocationOutput(&DredPageFaultOutput));
            __debugbreak();
#endif            
            LOG(ERROR) << "Device removal (TDR) has been detected. Reason:";
            LOG_SYSRESULT(pDevice->GetDeviceRemovedReason());            
            return;
        } 
        CHECK_HR(hr);
    }
    Device::Device(DeviceDesc cfg) {
#ifdef _DEBUG    
        ComPtr<ID3D12Debug> debugInterface;
        CHECK_HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)));
        debugInterface->EnableDebugLayer();
        CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&m_Factory));

        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> pDredSettings;
        CHECK_HR(D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings)));

        // Turn on AutoBreadcrumbs and Page Fault reporting
        pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
#else
        CreateDXGIFactory2(NULL, IID_PPV_ARGS(&dxgiFactory));
#endif
        m_Adapter = GetHardwareAdapter(m_Factory.Get(), cfg.AdapterIndex);
        CHECK_HR(D3D12CreateDevice(m_Adapter.Get(), D3D_MIN_FEATURE_LEVEL, IID_PPV_ARGS(&m_Device)));
        CheckDeviceCapabilities(m_Device.Get());
        LOG(INFO) << "D3D12 Device created";
        LogDeviceInformation(m_Device.Get());
        {
            using enum CommandListType;
            // create the command queues
            m_CommandQueues.resize(+NUM_TYPES);
            m_CommandQueues[+Direct] = std::make_unique<CommandQueue>(this, Direct);
            m_CommandQueues[+Direct]->SetName(L"Direct Command Queue");
            m_CommandQueues[+Copy] = std::make_unique<CommandQueue>(this, Copy);
            m_CommandQueues[+Copy]->SetName(L"Copy Command Queue");
            m_CommandQueues[+Compute] = std::make_unique<CommandQueue>(this, Compute);
            m_CommandQueues[+Compute]->SetName(L"Compute Command Queue");
        }
        {
            using enum IndirectArgumentType;
            m_CommandSignatures.resize(+NUM_TYPES);
            m_CommandSignatures[+DARW] = std::make_unique<CommandSignature>(this, DARW);
            m_CommandSignatures[+DISPATCH] = std::make_unique<CommandSignature>(this, DISPATCH);
            m_CommandSignatures[+DISPATCH_MESH] = std::make_unique<CommandSignature>(this, DISPATCH_MESH);
        }
        // allocators
        D3D12MA::ALLOCATOR_DESC allocatorDesc{};
        allocatorDesc.pDevice = m_Device.Get();
        allocatorDesc.pAdapter = m_Adapter.Get();
        allocatorDesc.PreferredBlockSize = 0;
        D3D12MA::CreateAllocator(&allocatorDesc, &m_Allocator);
        {
            using enum ResourcePoolType;
            m_AllocatorPools.resize(+NUM_TYPES);
            // default pool is just nullptr
            m_AllocatorPools[+Default].Reset();
            // create the intermediate pool
            auto& intermediate = m_AllocatorPools[+Intermediate];
            D3D12MA::POOL_DESC poolDesc{};
            poolDesc.HeapProperties.Type = D3D12_HEAP_TYPE_UPLOAD;
            poolDesc.Flags = D3D12MA::POOL_FLAG_ALGORITHM_LINEAR;        
            m_Allocator->CreatePool(&poolDesc, intermediate.GetAddressOf());
            intermediate->SetName(L"Intermediate Pool");
        }
    }
    std::shared_ptr<Buffer> Device::AllocateIntermediateBuffer(Buffer::BufferDesc const& desc) {
        DCHECK(desc.poolType == ResourcePoolType::Intermediate);

        auto buffer = std::make_shared<Buffer>(this, desc);
        m_IntermediateBuffers.push_back(buffer);
        buffer->SetName(std::format(L"Intermediate Buffer #{}", m_IntermediateBuffers.size()));
        return m_IntermediateBuffers.back();
    }
    void Device::FlushIntermediateBuffers() {
        for (auto& buffer : m_IntermediateBuffers) {
            CHECK(buffer.use_count() == 1); // ensure intermediate buffers are un-refed before release
        }
        m_IntermediateBuffers.clear();
    }
    Device::~Device() {
        m_Factory->Release();        
    }
    /* Helper functions */
    void Device::CreateRawBufferShaderResourceView(Buffer* buffer, Descriptor* desc) {        
        CHECK(buffer->GetDesc().isRawBuffer());
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.StructureByteStride = 0;
        srvDesc.Buffer.NumElements = buffer->GetDesc().width / sizeof(float);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_Device->CreateShaderResourceView(*buffer, &srvDesc, desc->get_cpu_handle());
    }
    void Device::CreateStructedBufferShaderResourceView(Buffer* buffer, Descriptor* desc) {
        CHECK(!buffer->GetDesc().isRawBuffer());
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;        
        srvDesc.Buffer.StructureByteStride = buffer->GetDesc().stride;;
        srvDesc.Buffer.NumElements = buffer->GetDesc().width / buffer->GetDesc().stride;       
        m_Device->CreateShaderResourceView(*buffer, &srvDesc, desc->get_cpu_handle());
    }
    void Device::CreateDepthStencilView(Texture* texture, Descriptor* desciptor) {
        D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
        desc.Format = DXGI_FORMAT_D32_FLOAT;
        desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        m_Device->CreateDepthStencilView(*texture, &desc, desciptor->get_cpu_handle());
    }
    void Device::CreateConstantBufferView(Buffer* buffer, Descriptor* descriptor) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
        desc.BufferLocation = buffer->GetGPUAddress();
        desc.SizeInBytes = buffer->GetDesc().sizeInBytes();
        m_Device->CreateConstantBufferView(&desc, descriptor->get_cpu_handle());
    };
}