#include "D3D12Swapchain.hpp"
#include "D3D12Device.hpp"
namespace RHI {
    Swapchain::Swapchain(Device* device, SwapchainDesc const& cfg) : DeviceChild(device) {
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.BufferCount = cfg.BackBufferCount;
        swapChainDesc.Width = cfg.InitWidth;
        swapChainDesc.Height = cfg.InitHeight;
        swapChainDesc.Format = ResourceFormatToD3DFormat(cfg.Format);
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;
        ComPtr<IDXGISwapChain1> swapChain;
        CHECK_HR(device->GetDXGIFactory()->CreateSwapChainForHwnd(
            *device->GetCommandQueue<CommandListType::Direct>(), cfg.Window, &swapChainDesc, nullptr, nullptr, &swapChain
        ));
        CHECK_HR(device->GetDXGIFactory()->MakeWindowAssociation(cfg.Window, NULL));
        CHECK_HR(swapChain.As(&m_Swapchain));
        m_FrameFence = std::make_unique<Fence>(device);
        m_FrameFence->SetName(L"Swap Fence");
        nFenceValues.resize(cfg.BackBufferCount);
        m_BackbufferFormat = cfg.Format;
        Resize(cfg.InitWidth, cfg.InitHeight);
    }
    void Swapchain::Present(bool vsync) {
        HRESULT hr = m_Swapchain->Present(vsync, 0);
        CHECK_DEVICE_REMOVAL(m_Device, hr);
        nBackbufferIndex = m_Swapchain->GetCurrentBackBufferIndex();
        nFrameCount++;
    }
    void Swapchain::PresentAndMoveToNextFrame(bool vsync) {
        // Signal after this command queue is executed, i.e. this backbuffer is available again
        auto gfxQueue = m_Device->GetCommandQueue<CommandListType::Direct>();
        gfxQueue->Signal(m_FrameFence.get(), nFenceValues[nBackbufferIndex]);
        // Flip to the next BB for our next command list
        Present(vsync);
        // Wait for the next BB to be ready
        m_FrameFence->Wait(nFenceValues[nBackbufferIndex]);
        // Increment the fence value which should be monotonously increasing upon any backbuffers' completion    
        nFenceValues[nBackbufferIndex] = gfxQueue->GetUniqueFenceValue();
    }
    void Swapchain::Resize(uint width, uint height) {
        nWidth = width;
        nHeight = height;
        uint buffers = (UINT)m_Backbuffers.size();        
        // Reset fence values
        size_t resetFenceValue = m_Device->GetCommandQueue<CommandListType::Direct>()->GetUniqueFenceValue();
        for (uint i = 0; i < buffers; i++)
            nFenceValues[i] = resetFenceValue;        
        DXGI_SWAP_CHAIN_DESC desc = {};
        m_Swapchain->GetDesc(&desc);
        CHECK_HR(m_Swapchain->ResizeBuffers(buffers, nWidth, nHeight, desc.BufferDesc.Format, desc.Flags));
        CHECK_HR(m_Swapchain->GetFullscreenState(&bIsFullscreen, nullptr));
        nBackbufferIndex = m_Swapchain->GetCurrentBackBufferIndex();
        // Recreate the BBs
        m_Backbuffers.clear();
        for (uint i = 0; i < desc.BufferCount; i++) {            
            ComPtr<ID3D12Resource> backbuffer;
            CHECK_HR(m_Swapchain->GetBuffer(i, IID_PPV_ARGS(backbuffer.GetAddressOf())));
            auto name = std::format(L"Backbuffer #{}", i);
            m_Backbuffers.push_back(std::make_unique<Texture>(m_Device, std::move(backbuffer), name.c_str()));            
        }        
        // RTVs
        // Free previous RTVs (if any)
        for (auto& rtv : m_BackbufferRTVs) rtv.descriptor.release();        
        m_BackbufferRTVs.clear();
        for (auto& backbuffer : m_Backbuffers) {            
            m_BackbufferRTVs.push_back(RenderTargetView(backbuffer.get()));
        }        
    }
}