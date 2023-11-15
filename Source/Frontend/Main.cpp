#include "../pch.hpp"
#include "ViewportWindow.hpp"

#include "../Backend/RHI/RHI.hpp"
#include "../Backend/IO/AssetManager.hpp"
#include "../Backend/Scene/SceneGraph.hpp"
#include "../Backend/Scene/SceneGraphView.hpp"
using namespace IO;
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
        1600,1000, ResourceFormat::R8G8B8A8_UNORM, 2, vp.m_hWnd
    });

    AssetManager assets;
    SceneGraph scene{ assets };
    SceneGraphView sceneView(&device, scene);

    Assimp::Importer importer;   
    auto imported = importer.ReadFile("..\\Resources\\DefaultCube.glb", aiProcess_Triangulate | aiProcess_ConvertToLeftHanded);
    scene.load_from_aiScene(imported);
    CommandList uploadCmd(&device, CommandListType::Copy, 1);
    
    uploadCmd.Begin();
    device.BeginUpload(&uploadCmd);
    assets.upload_all<StaticMeshAsset>(&device);
    uploadCmd.End();
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
    }
}