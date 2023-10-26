#include "D3D12Device.hpp"
#ifdef RHI_D3D12_USE_AGILITY
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 4; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }
#endif
static void D3DGetHardwareAdapter(IDXGIFactory1* pFactory, IDXGIAdapter1** ppAdapterOut, UINT adapterIndex = 0) {
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
            if (count >= adapterIndex) return;
            count++;
        }

    }
    LOG(FATAL) << "No available D3D12 hardware found!";
    abort();
}
namespace RHI {
    Descriptor Device::CreateRenderTargetView(Texture* tex) {
        auto handle = GetCpuAllocator(DescriptorHeap::HeapType::RTV)->Allocate();
        m_Device->CreateRenderTargetView(*tex, nullptr, handle.cpu_handle);
        return handle;
    }
    void Device::CreateSwapchainAndBackbuffers(Swapchain::SwapchainConfig const& cfg) {
        m_SwapChain = std::make_unique<Swapchain>(this, cfg);
        m_SwapChain->Resize(this, cfg.InitWidth, cfg.InitHeight);
        m_CommandLists[CommandList::CommandListType::DIRECT] = std::make_unique<CommandList>(
            this,
            CommandList::CommandListType::DIRECT,
            cfg.BackBufferCount
        );
    }
    Device::Device(DeviceConfig cfg) {
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
        ComPtr<IDXGIAdapter1> hardwareAdapter;
        D3DGetHardwareAdapter(m_Factory.Get(), &hardwareAdapter, cfg.AdapterIndex);
        CHECK_HR(D3D12CreateDevice(hardwareAdapter.Get(), D3D_MIN_FEATURE_LEVEL, IID_PPV_ARGS(&m_Device)));
        m_CommandQueue = std::make_unique<CommandQueue>(this);
        m_DescriptorHeapAllocators.resize(DescriptorHeap::HeapType::NUM_TYPES);
        m_DescriptorHeapAllocators[DescriptorHeap::HeapType::RTV] = std::make_unique<DescriptorHeapAllocator>(
            this,
            DescriptorHeap::DescriptorHeapConfig{
            .ShaderVisible = false,
                .DescriptorCount = ALLOC_SIZE_DESC_HEAP,
                .Type = DescriptorHeap::HeapType::RTV
        });
        m_CommandLists.resize(CommandList::CommandListType::NUM_TYPES);
        m_MarkerFence = std::make_unique<MarkerFence>(this);
    }
    void Device::BeginFrame() {
        auto directList = GetCommandList(CommandList::DIRECT);
        directList->ResetAllocator(m_SwapChain->nBackbufferIndex);
        directList->Begin(m_SwapChain->nBackbufferIndex);
    }
    void Device::EndFrame() {
        auto list = GetCommandList(CommandList::DIRECT);
        list->End();
        m_CommandQueue->Execute(list);
        m_SwapChain->PresentAndMoveToNextFrame(m_CommandQueue.get(), 0);
    }
    void Device::WaitForGPU() {
        m_MarkerFence->MarkAndWait(m_CommandQueue.get());
    }
}