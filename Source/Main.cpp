#include "pch.hpp"
#include "Win32/ViewportWindow.hpp"
#include "Renderer/Renderer/Renderer.hpp"

int main(int argc, char* argv[]) {    
    FLAGS_alsologtostderr = true;
    google::InitGoogleLogging(argv[0]);
    
    CHECK(SetProcessDPIAware());
    CHECK(SetConsoleOutputCP(65001));

    ViewportWindow vp;
    vp.Create(GetModuleHandle(NULL), 1920, 1080, L"ViewportWindowClass", L"Viewport");
    
    Renderer renderer;
    renderer.Init(
        Device::DeviceConfig{.AdapterIndex = 0},
        Swapchain::SwapchainConfig{.InitWidth = 1920, .InitHeight = 1080, .BackBufferCount = 2, .Window = vp.m_hWnd }
    );       
    MSG  msg{};
    while (1) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE) != 0)
        {
            // Translate and dispatch the message
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else {
            renderer.BeginFrame();
            // Populate command list
            auto D3DcmdList = renderer.GetDevice()->GetCommandList(CommandList::DIRECT)->operator ID3D12GraphicsCommandList6 * ();
            auto swapChain = renderer.GetDevice()->GetSwapchain();
            auto bbIndex = swapChain->nBackbufferIndex;

            // Indicate that the back buffer will be used as a render target.
            auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                *swapChain->GetBackbuffer(bbIndex),
                D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET
            );
            D3DcmdList->ResourceBarrier(1, &barrier);

            auto rtvHandle = swapChain->GetBackbufferRTV(bbIndex);
            D3DcmdList->OMSetRenderTargets(1, &rtvHandle.cpu_handle, FALSE, nullptr);

            const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
            D3DcmdList->ClearRenderTargetView(rtvHandle.cpu_handle, clearColor, 0, nullptr);
            
            // Indicate that the back buffer will now be used to present.
            barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                *swapChain->GetBackbuffer(bbIndex),
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT
            );
            D3DcmdList->ResourceBarrier(1, &barrier);

            renderer.EndFrame();
        }
    }
}