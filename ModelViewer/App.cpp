#include "App.hpp"
#include "Mesh.hpp"
#include "Scene.hpp"
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
        m_scene = ConstructUnique<Scene>(
            GetAllocator(),
            m_device.Get(),
            m_swapchain->GetImages().size(),
            SceneBudgets{},
            GetAllocator()
        );
        auto meshData = LoadMeshFromObjFile("data/assets/Cube.obj", GetAllocator());
        mesh = m_scene->CreateMesh(meshData.m_vertex_data, meshData.m_index_data);
    }
    void App::OnBeforeFrame()
    {
        m_scene->OnBeforeFrame(m_renderer->GetSync());
    }
    void App::OnApplicationTick()
    {
        WaitForFrame();
        size_t total = 100;
        auto data = m_scene->MapInstanceData<InstanceMetadata>();
        CHECK_MSG(data.size() >= total, "Not enough space (max={}, current={})", data.size(),total);
        auto mesh_id = m_scene->GetMesh(mesh).primitiveID;
        auto time = GetApplicationTime<float>();
        size_t sq = sqrt(total);
        // Camera
        float4x4 view = lookAt(
                    vec3(sq,sq,sq),
                    vec3(sq / 2,sq / 2, 0),
                    vec3(0.0f, 0.0f, 1.0f));
        float4x4 proj = infinitePerspective(radians(45.0f),  GetSwapchain()->GetAspectRatio(),0.1f);
        proj[1][1] *= -1; // vulkan NDC
        m_camera = proj * view;
        for (size_t instance = 0; instance < total; ++instance)
        {
            float theta = time * acos(-1) * 0.1f;
            quat q = quat(cosf(theta), float3(0,1,0) * sinf(theta));
            data[instance] = {
                .primitiveID = mesh_id + 1,
                .t = float3{
                    (instance / sq),
                    (instance % sq),
                    0 // sin(time + instance  * acos(-1) / total) * sq / 8
                },
                .q = float4(q.x, q.y, q.z, q.w)
            };
        }
        m_scene->UnmapInstanceData();
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
