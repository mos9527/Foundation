#include "D3D12Swapchain.hpp"
#include "D3D12Device.hpp"
namespace RHI {
    Swapchain::Swapchain(Device* device, SwapchainConfig const& cfg) {
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.BufferCount = cfg.BackBufferCount;
        swapChainDesc.Width = cfg.InitWidth;
        swapChainDesc.Height = cfg.InitHeight;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;
        ComPtr<IDXGISwapChain1> swapChain;
        CHECK_HR(device->GetDXGIFactory()->CreateSwapChainForHwnd(
            *device->GetCommandQueue(), cfg.Window, &swapChainDesc, nullptr, nullptr, &swapChain
        ));
        CHECK_HR(device->GetDXGIFactory()->MakeWindowAssociation(cfg.Window, DXGI_MWA_NO_ALT_ENTER));
        CHECK_HR(swapChain.As(&m_Swapchain));
    }
    void Swapchain::CreateBackBuffers(Device* device) {        
        m_Backbuffers.clear();
        DXGI_SWAP_CHAIN_DESC desc = {};
        m_Swapchain->GetDesc(&desc);
        for (UINT i = 0; i < desc.BufferCount; i++) {
            Texture::GpuTextureDesc desc{};
            ComPtr<ID3D12Resource> backbuffer;
            CHECK_HR(m_Swapchain->GetBuffer(i, IID_PPV_ARGS(backbuffer.GetAddressOf())));
            m_Backbuffers.push_back(std::make_unique<Texture>(desc, std::move(backbuffer)));            
        }
        nFenceValues.resize(desc.BufferCount);
        m_FrameFence = std::make_unique<Fence>(device);
    }
    void Swapchain::CreateRenderTargetViews(Device* device) {
        // Free previous RTVs (if any)
        for (auto& handle : m_BackbufferRTVs) {
            device->GetCpuAllocator(DescriptorHeap::HeapType::RTV)->Free(handle.handle);
        }
        m_BackbufferRTVs.clear();
        for (auto& backbuffer : m_Backbuffers) {
            m_BackbufferRTVs.push_back(device->CreateRenderTargetView(backbuffer.get()));
        }
    }
    void Swapchain::Present(bool vsync) {
        CHECK_HR(m_Swapchain->Present(vsync, 0));
        nBackbufferIndex = m_Swapchain->GetCurrentBackBufferIndex();
    }    
    void Swapchain::PresentAndMoveToNextFrame(CommandQueue* signalQueue, bool vsync) {
        // Signal after this command queue is executed, i.e. this backbuffer is available again
        signalQueue->Signal(m_FrameFence.get(), nFenceValues[nBackbufferIndex]);
        // Flip to the next BB for our next command list
        Present(vsync);
        // Wait for the next BB to be ready
        m_FrameFence->Wait(nFenceValues[nBackbufferIndex]);
        // Increment the fence value which should be monotonously increasing upon any backbuffers' completion    
        nFenceValues[nBackbufferIndex] = ++nFenceValue;
    }
    void Swapchain::Resize(Device* device, UINT width, UINT height) {
        UINT buffers = (UINT)m_Backbuffers.size();
        // Reset BBs and their fence values
        for (UINT i = 0; i < buffers; i++) {
            m_Backbuffers[i]->Reset();
            nFenceValues[i] = nFenceValue;
        }
        DXGI_SWAP_CHAIN_DESC desc = {};
        m_Swapchain->GetDesc(&desc);
        CHECK_HR(m_Swapchain->ResizeBuffers(buffers, width, height, desc.BufferDesc.Format, desc.Flags));
        CHECK_HR(m_Swapchain->GetFullscreenState(&bIsFullscreen, nullptr));
        nBackbufferIndex = m_Swapchain->GetCurrentBackBufferIndex();
        // Recreate the BBs & RTVs
        CreateBackBuffers(device);
        CreateRenderTargetViews(device);
    }
}