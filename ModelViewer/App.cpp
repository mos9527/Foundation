#include "App.hpp"
#include <Rendering/GPUScene.hpp>
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
        mScene = ConstructUnique<GPUScene>(
            GetAllocator(),
            mDevice.Get(),
            mSwapchain->GetImages().size(),
            GPUSceneBudgets{},
            GetAllocator()
        );
        auto meshData = meshLoadObjFile("data/assets/Sphere.obj", GetAllocator());
        mesh = mScene->PushMesh(meshData.m_vertex_data, meshData.m_index_data);
    }
    void App::OnBeforeFrame()
    {
        mScene->OnBeforeFrame(mRenderer->GetSync());
    }
    void App::OnApplicationTick()
    {
        WaitForFrame();
        size_t total = 8 * 8; // 0.6mil instances
        auto data = mScene->MapInstanceData<InstanceData>();
        CHECK_MSG(data.size() >= total, "Not enough space (max={}, current={})", data.size(),total);
        auto time = GetApplicationTime<float>();
        size_t sq = sqrt(total);
        // Camera
        float4x4 view = lookAt(
                    vec3(sq,sq,sq),
                    vec3(sq / 2,sq / 2, 0),
                    vec3(0.0f, 0.0f, 1.0f));
        float4x4 proj = infinitePerspective(radians(45.0f),  GetSwapchain()->GetAspectRatio(),0.1f);
        proj[1][1] *= -1; // vulkan NDC
        mCamera = proj * view;
        for (size_t instance = 0; instance < total; ++instance)
        {
            float theta = time * acos(-1) * 0.1f;
            quat q = quat(cosf(theta / 2), float3(0,1,0) * sinf(theta / 2));
            data[instance] = {
                .primitiveOffsetPP = mScene->QueryMesh(mesh).primitiveOffset + 1,
                .t = float3{
                    (instance / sq),
                    (instance % sq),
                    sin(instance + time)
                },
                .q = float4(q.x, q.y, q.z, q.w)
            };
        }
        mScene->UnmapInstanceData();
    }
}
using namespace ModelViewer;
using namespace Foundation::Async;
int main(int argc, char** argv) {
    App app;
    app.Initialize<VulkanApplication>({
        .windowTitle = "Model Viewer",
        .present = true,
        .asyncCompute = true
    });
    app.RunForever();
}
