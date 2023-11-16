#include "../pch.hpp"
#include "ViewportWindow.hpp"

#include "../Backend/RHI/RHI.hpp"
#include "../Backend/AssetRegistry/AssetRegistry.hpp"
#include "../Backend/SceneGraph/SceneGraph.hpp"
#include "../Backend/SceneGraph/SceneGraphView.hpp"

#include "../../Dependencies/imgui/imgui.h"
using namespace RHI;

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
        }
    }
}