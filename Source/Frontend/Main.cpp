#include "../pch.hpp"
#include "ViewportWindow.hpp"

#include "../Backend/RHI/RHI.hpp"
#include "../Backend/AssetRegistry/AssetRegistry.hpp"
#include "../Backend/SceneGraph/SceneGraph.hpp"
#include "../Backend/SceneGraph/SceneGraphView.hpp"

#include "../../Dependencies/imgui/imgui.h"
#include "../../Dependencies/imgui/backends/imgui_impl_dx12.h"
#include "../../Dependencies/imgui/backends/imgui_impl_win32.h"

#include "Renderer/Deferred.hpp"
using namespace RHI;
// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int main(int argc, char* argv[]) {    
    FLAGS_alsologtostderr = true;
    google::InitGoogleLogging(argv[0]);
    
    CHECK(SetProcessDPIAware());
    CHECK(SetConsoleOutputCP(65001));

    ViewportWindow vp;
    vp.Create(1600, 1000, L"ViewportWindowClass", L"Viewport");

    RHI::Device device(Device::DeviceDesc{.AdapterIndex = 0});
    RHI::Swapchain swapchain(&device, Swapchain::SwapchainDesc {
        vp.m_hWnd, 1600,1000, ResourceFormat::R8G8B8A8_UNORM
    });

    AssetRegistry assets;
    SceneGraph scene{ assets };
    SceneGraphView sceneView(&device, scene);
    DeferredRenderer renderer(assets, scene, &device, &swapchain);

    

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(vp.m_hWnd);
    auto pSrvHeap = device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap();
    ImGui_ImplDX12_Init(device.GetNativeDevice(), RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT,
        ResourceFormatToD3DFormat(swapchain.GetFormat()), pSrvHeap,
        pSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        pSrvHeap->GetGPUDescriptorHandleForHeapStart());
    vp.SetCallback([&](HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
        ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
        if (message == WM_SIZE) {
            device.Wait();
            swapchain.Resize(std::max((WORD)128u, LOWORD(lParam)), std::max((WORD)128u, HIWORD(lParam)));
        }
    });
    float baseFontSize = 16.0f * ImGui_ImplWin32_GetDpiScaleForHwnd(vp.m_hWnd);       
    ImGui::GetIO().Fonts->AddFontFromFileTTF(
        "../Resources/Fonts/DroidSansFallback.ttf",
        baseFontSize,
        NULL,
        ImGui::GetIO().Fonts->GetGlyphRangesChineseFull()
    );
    ImGui::StyleColorsLight();

    Assimp::Importer importer;   
    auto imported = importer.ReadFile("..\\Resources\\DefaultCube.glb", aiProcess_Triangulate | aiProcess_ConvertToLeftHanded);
    scene.load_from_aiScene(imported);
    CommandList* uploadCmd = device.GetCommandList<CommandListType::Copy>();    
    uploadCmd->Begin();
    device.BeginUpload(uploadCmd);
    assets.upload_all<StaticMeshAsset>(&device);
    uploadCmd->End();
    LogD3D12MABudget(device.GetAllocator());
    device.CommitUpload().Wait();
    LOG(INFO) << "Resources uploaded";
    device.Clean();
    LogD3D12MABudget(device.GetAllocator());
    LOG(INFO) << "Cleaned...let's do it.";

    scene.log_entites();
    sceneView.update();

    bool vsync = false;
    auto cmd = device.GetCommandList<CommandListType::Direct>();
    auto render = [&]() {
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::ShowDemoWindow();
        ImGui::Render();
        uint bbIndex = swapchain.GetCurrentBackbufferIndex();
        cmd->ResetAllocator(bbIndex);
        cmd->Begin(bbIndex);
        swapchain.GetBackbuffer(bbIndex)->SetBarrier(cmd, ResourceState::RenderTarget);
        auto rtv = swapchain.GetBackbufferRTV(bbIndex);
        /* FRAME BEGIN */
        const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
        cmd->GetNativeCommandList()->ClearRenderTargetView(rtv.descriptor.get_cpu_handle(), clearColor, 0, nullptr);
        // Render
        renderer.Render();
        // ImGui
        cmd->GetNativeCommandList()->OMSetRenderTargets(1, &rtv.descriptor.get_cpu_handle(), FALSE, nullptr);
        auto const srvHeap = device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap();
        cmd->GetNativeCommandList()->SetDescriptorHeaps(1, &srvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd->GetNativeCommandList());
        /* FRAME END */
        swapchain.GetBackbuffer(bbIndex)->SetBarrier(cmd, ResourceState::Present);
        cmd->End();
        cmd->Execute();
        swapchain.PresentAndMoveToNextFrame(false);
    };

    // win32 message pump
    MSG  msg{};
    while (1) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE) != 0)
        {
            // Translate and dispatch the message
            TranslateMessage(&msg);
            DispatchMessage(&msg);            
        }
        else {
            // Render!
            render();
        }
    }
}