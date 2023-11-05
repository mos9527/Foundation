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

        D3D12_FEATURE_DATA_D3D12_OPTIONS7 features = {};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &features, sizeof(features)))
            || (features.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED))
        {
            LOG(FATAL) << "ERROR: Mesh Shaders aren't supported on this device";
        }
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

    std::shared_ptr<Descriptor> Device::GetRenderTargetView(Texture* tex) {
        auto handle = GetDescriptorHeap(DescriptorHeapType::RTV)->GetDescriptor();
        m_Device->CreateRenderTargetView(*tex, nullptr, handle->get_cpu_handle());
        return handle;
    }
    std::shared_ptr<Descriptor> Device::GetBufferShaderResourceView(Buffer* buf, ResourceFormat format) {
        bool is_raw = buf->GetDesc().stride == 0;
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = is_raw ? DXGI_FORMAT_R32_TYPELESS : ResourceFormatToD3DFormat(buf->GetDesc().format);
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.StructureByteStride = buf->GetDesc().stride;
        srvDesc.Buffer.NumElements = buf->GetDesc().width / (is_raw ? sizeof(float) : buf->GetDesc().stride);
        if (is_raw) srvDesc.Buffer.Flags |= D3D12_BUFFER_SRV_FLAG_RAW;
        auto handle = GetDescriptorHeap(DescriptorHeapType::CBV_SRV_UAV)->GetDescriptor();
        m_Device->CreateShaderResourceView(*buf, &srvDesc, handle->get_cpu_handle());
        return handle;
    }
    std::shared_ptr<Descriptor> Device::GetTexture2DShaderResourceView(Buffer* buf, ResourceDimensionSRV view) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = ResourceFormatToD3DFormat(buf->GetDesc().format);
        srvDesc.ViewDimension = (D3D12_SRV_DIMENSION)view;
        srvDesc.Texture2D.MipLevels = buf->GetDesc().mipLevels;
        auto handle = GetDescriptorHeap(DescriptorHeapType::CBV_SRV_UAV)->GetDescriptor();
        m_Device->CreateShaderResourceView(*buf, &srvDesc, handle->get_cpu_handle());
        return handle;
    }
    std::shared_ptr<Descriptor> Device::GetConstantBufferView(Buffer* buf) {
        CHECK(buf->GetDesc().dimension == ResourceDimension::Buffer);
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
        cbvDesc.BufferLocation = buf->GetGPUAddress();
        cbvDesc.SizeInBytes = buf->GetDesc().width;
        auto handle = GetDescriptorHeap(DescriptorHeapType::CBV_SRV_UAV)->GetDescriptor();
        m_Device->CreateConstantBufferView(&cbvDesc, handle->get_cpu_handle());
        return handle;
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
        CheckDeviceCapabilities(m_Device.Get());
        LOG(INFO) << "D3D12 Device created";
        LogDeviceInformation(m_Device.Get());
        {
            using enum CommandListType;
            // create the command queues
            m_CommandQueues.resize(+NUM_TYPES);
            m_CommandQueues[+Direct] = std::make_unique<CommandQueue>(this, Direct);
            m_CommandQueues[+Copy] = std::make_unique<CommandQueue>(this, Copy);
            m_CommandQueues[+Compute] = std::make_unique<CommandQueue>(this, Compute);
        }
        {
            using enum DescriptorHeapType;
            // initialize descriptor heaps we'll be using
            m_DescriptorHeaps.resize(+NUM_TYPES);
            m_DescriptorHeaps[+RTV] = std::make_unique<DescriptorHeap>(
                this,
                DescriptorHeap::DescriptorHeapDesc{
                .shaderVisible = false,
                    .descriptorCount = ALLOC_SIZE_DESC_HEAP,
                    .heapType = RTV
            });
            m_DescriptorHeaps[+RTV]->SetName(L"RTV Heap");
            m_DescriptorHeaps[+CBV_SRV_UAV] = std::make_unique<DescriptorHeap>(
                this,
                DescriptorHeap::DescriptorHeapDesc{
                .shaderVisible = true,
                    .descriptorCount = ALLOC_SIZE_DESC_HEAP,
                    .heapType = CBV_SRV_UAV
            });
            m_DescriptorHeaps[+CBV_SRV_UAV]->SetName(L"CBV-SRV-UAV Heap");
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
        m_IntermediateBuffers.clear();
    }
    Device::~Device() {
        m_Factory->Release();        
    }
}