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
        meshLoadObjFile(vertices, indices, "data/assets/Sphere.obj");
        SceneMeshData meshData = sceneMeshDataFromVertexIndex(vertices, indices, GetAllocator());
        mesh = mScene->PushMesh(meshData);
    }    
    void App::OnApplicationTick()
    {
        auto instances = mScene->MapInstances();
        instances[0].meshAllocationRawOffsetPP = mScene->QueryMesh(mesh).selfRawOffset + 1;
        instances[0].q = float4(0, 0, 0, 1);
        mScene->UnmapInstances();
    }
    // TEST: Limit FPS for debug
    void App::OnBeforeFrame() {
        // nop
    }
} // namespace ModelViewer
using namespace ModelViewer;
using namespace Foundation::Async;
int main(int argc, char** argv)
{
    App app;
    app.Initialize<VulkanApplication>({.windowTitle = "Model Viewer", .present = true, .asyncCompute = true, .vsync = true});
    app.RunForever();
}
