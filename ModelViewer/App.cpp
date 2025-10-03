#include "App.hpp"
#include "Assets/Mesh.hpp"
using namespace Foundation;
using namespace Foundation::Core;
using namespace Foundation::RenderCore;
using namespace Foundation::Native;
namespace ModelViewer
{
    /* -- States -- */
    SceneHandle mesh;
    void App::OnDeviceSetup()
    {
        mGPUScene = ConstructUnique<GPUScene>(GetAllocator(), mDevice.Get(), GPUSceneBudgets{}, GetAllocator());
        mScene = ConstructUnique<Scene>(GetAllocator(), mGPUScene.get(), GetAllocator());
        // TEST: Load mesh
        Vector<MeshVertex> vertices(GetAllocator());
        Vector<MeshIndex> indices(GetAllocator());
        meshLoadObjFile(vertices, indices, "data/assets/Kitten.obj");
        SceneMeshData meshData = sceneMeshDataFromVertexIndex(vertices, indices, GetAllocator());
        mesh = mScene->PushMesh(meshData);
    }    
    void App::OnApplicationTick()
    {
        mScene->mCamera.position = float3{1,1,1};
        auto instances = mScene->MapInstances();
        instances[0].meshAllocationRawOffsetPP = mScene->QueryMesh(mesh).selfRawOffset + 1;
        float time = GetApplicationTime();
        float theta = time * acos(-1) * 0.1f;
        instances[0].q = angleAxis(theta, float3{0, 0, 1}) * angleAxis(radians(90.0f), float3{1, 0, 0});
        mScene->UnmapInstances();
    }    
    void App::OnBeforeFrame() { 
        mScene->mCamera.aspectRatio = mSwapchain->GetAspectRatio();
    }
} // namespace ModelViewer
using namespace ModelViewer;
using namespace Foundation::Async;
int main(int argc, char** argv)
{
    App app;
    app.Initialize<VulkanApplication>({.windowTitle = "Model Viewer", .present = true, .asyncCompute = true, .vsync = false});
    app.RunForever();
}
