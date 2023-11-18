#include "../pch.hpp"
#include "ViewportWindow.hpp"

#include "../Runtime/RHI/RHI.hpp"
#include "../Runtime/AssetRegistry/AssetRegistry.hpp"
#include "../Runtime/SceneGraph/SceneGraph.hpp"
#include "../Runtime/SceneGraph/SceneGraphView.hpp"

#ifdef IMGUI_ENABLED
#include "../../Dependencies/imgui/imgui.h"
#include "../../Dependencies/imgui/backends/imgui_impl_dx12.h"
#include "../../Dependencies/imgui/backends/imgui_impl_win32.h"
#endif

#include "../Runtime/Renderer/Deferred.hpp"
using namespace RHI;
#ifdef IMGUI_ENABLED
// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
int main(int argc, char* argv[]) {    
    FLAGS_alsologtostderr = true;
    google::InitGoogleLogging(argv[0]);
    
    CHECK(SetProcessDPIAware());
    CHECK(SetConsoleOutputCP(65001));

    ViewportWindow vp;
    vp.Create(1600, 1000, L"ViewportWindowClass", L"Viewport");

    std::mutex renderMutex;
    RHI::Device device(Device::DeviceDesc{.AdapterIndex = 0});
    RHI::Swapchain swapchain(&device, Swapchain::SwapchainDesc {
        vp.m_hWnd, 1600,1000, ResourceFormat::R8G8B8A8_UNORM
    });

    DefaultTaskThreadPool taskpool;
    AssetRegistry assets;
    SceneGraph scene{ assets };
    SceneGraphView sceneView(&device, scene);
    DeferredRenderer renderer(assets, scene, &device, &swapchain);
    scene.set_active_camera(scene.create_child_of<CameraComponent>(scene.get_root()));
    auto& camera = scene.get_active_camera();
#ifdef IMGUI_ENABLED
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(vp.m_hWnd);

    Descriptor ImGuiFontDescriptor = device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
    ImGui_ImplDX12_Init(device.GetNativeDevice(), RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT,
        ResourceFormatToD3DFormat(swapchain.GetFormat()), *device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>(),
        ImGuiFontDescriptor,
        ImGuiFontDescriptor
    ); // ImGui won't use more than this one descriptor
    float baseFontSize = 16.0f * ImGui_ImplWin32_GetDpiScaleForHwnd(vp.m_hWnd);
    ImGui::GetIO().Fonts->AddFontFromFileTTF(
        "../Resources/Fonts/DroidSansFallback.ttf",
        baseFontSize,
        NULL,
        ImGui::GetIO().Fonts->GetGlyphRangesChineseFull()
    );
    ImGui::StyleColorsLight();
#endif
    vp.SetCallback([&](HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
#ifdef IMGUI_ENABLED
        ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
#endif
        if (message == WM_SIZE) {
            device.Wait();
            swapchain.Resize(std::max((WORD)128u, LOWORD(lParam)), std::max((WORD)128u, HIWORD(lParam)));
        }
    });

    SyncFence uploadFence;
    taskpool.push([&] {
        LOG(INFO) << "Loading scene";
        Assimp::Importer importer;
        path_t filepath = L"..\\Resources\\glTF-Sample-Models\\2.0\\FlightHelmet\\glTF\\FlightHelmet.gltf";
        std::string u8path = (const char*)filepath.u8string().c_str();
        auto imported = importer.ReadFile(u8path, aiProcess_Triangulate | aiProcess_ConvertToLeftHanded);
        scene.load_from_aiScene(imported, filepath.parent_path());
        scene.log_entites();
        LOG(INFO) << "Requesting upload";
        std::scoped_lock lock(renderMutex);
        CommandList* cmd = device.GetCommandList<CommandListType::Copy>();
        device.BeginUpload(cmd);
        assets.upload_all<StaticMeshAsset>(&device);
        assets.upload_all<SDRImageAsset>(&device);
        uploadFence = device.EndUpload();
    });
    // xxx cleaning

    bool vsync = false;
    auto cmd = device.GetCommandList<CommandListType::Direct>();
    auto render = [&]() {
        std::scoped_lock lock(renderMutex);
#ifdef IMGUI_ENABLED
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();        
#endif
        uint bbIndex = swapchain.GetCurrentBackbufferIndex();
        cmd->ResetAllocator(bbIndex);
        cmd->Begin(bbIndex);
        swapchain.GetBackbuffer(bbIndex)->SetBarrier(cmd, ResourceState::RenderTarget);
        auto& rtv = swapchain.GetBackbufferRTV(bbIndex);
        /* FRAME BEGIN */
        const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
        ID3D12DescriptorHeap* const heaps[] = {
            device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap()
            /*device.GetOnlineDescriptorHeap<DescriptorHeapType::SAMPLER>()->GetNativeHeap()*/ // xxx do we really need more than static samplers?
        };
        cmd->GetNativeCommandList()->SetDescriptorHeaps(1, heaps);
        cmd->GetNativeCommandList()->ClearRenderTargetView(rtv.descriptor.get_cpu_handle(), clearColor, 0, nullptr);
        // Render
        ShaderResourceView* pSrv = nullptr;
        if (uploadFence.IsCompleted())
            pSrv = renderer.Render();
#ifdef IMGUI_ENABLED
        ImGui::Begin("Viewport");
        if (pSrv) 
            ImGui::Image((ImTextureID)pSrv->descriptor.get_gpu_handle().ptr, ImVec2(swapchain.GetWidth(),swapchain.GetHeight()));
        pSrv = nullptr;
        ImGui::End();
#endif
        // ImGui
        ImGui::Render();
        cmd->GetNativeCommandList()->OMSetRenderTargets(1, &rtv.descriptor.get_cpu_handle(), FALSE, nullptr);
#ifdef IMGUI_ENABLED
        PIXBeginEvent(cmd->GetNativeCommandList(), 0, L"ImGui");
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd->GetNativeCommandList());
        PIXEndEvent(cmd->GetNativeCommandList());
#endif
        /* FRAME END */
        swapchain.GetBackbuffer(bbIndex)->SetBarrier(cmd, ResourceState::Present);
        cmd->End();
        cmd->Execute();
        swapchain.PresentAndMoveToNextFrame(vsync);
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
            camera.localTransform.rotation.AddRotationPitchYawRoll({ 0,0,0.001f });
        }
    }
}