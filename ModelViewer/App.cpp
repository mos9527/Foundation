#include "App.hpp"
#include "Mesh.hpp"
#include "Scene.hpp"
using namespace Foundation;
using namespace Foundation::Core;
using namespace Foundation::RenderCore;
using namespace Foundation::Native;
namespace ModelViewer
{
    void App::OnDeviceSetup()
    {
        m_scene = ConstructUnique<Scene>(
            GetAllocator(),
            m_device.Get(), GetAllocator(),
            m_swapchain->GetImages().size(),
            SceneBudgets{}
        );
    }
    void App::OnBeforeFrame()
    {
        m_scene->OnBeforeFrame(m_renderer->GetSync());
    }
    void App::OnApplicationTick()
    {
        WaitForFrame();
        if (m_meshes.empty())
            return; // No mesh loaded yet.
        size_t cnt = 1;
        size_t sq = sqrt(cnt);
        float4x4 view = lookAt(
                    vec3(sq,sq,sq),
                    vec3(sq / 2,sq / 2, 0),
                    vec3(0.0f, 0.0f, 1.0f));
        float4x4 proj = infinitePerspective(radians(45.0f),  GetSwapchain()->GetAspectRatio(),0.1f);
        proj[1][1] *= -1; // vulkan NDC
        m_camera = proj * view;
        SceneHandle mesh = m_meshes.back().get<SceneMeshLoadResult>()->primitiveID;
        // TODO: Ugly. We have to retrieve these even outside update scope
        //       We can go for atomic solutions - like a SPSC queue for these
        {
            for (size_t instance = 0; instance < cnt; ++instance)
            {
                m_scene->UpdateInstanceAsync(instance, {
                    .enabled = true,
                    .primitiveID = mesh,
                    .transform = translate(float3{
                        (instance / sq),
                        (instance % sq),
                        sin(GetApplicationTime<float>() + instance  * acos(-1) / cnt)
                    })
                });
            }
        }
    }
}
using namespace ModelViewer;
using namespace Foundation::Async;
int main(int argc, char** argv) {
    // Render (App) Thread
    App app;
    app.Initialize<VulkanApplication>({
        .windowTitle = "Model Viewer",
        .present = true,
        .asyncCompute = true
    });
    app.m_meshes.push_back(app.m_scene->LoadMeshAsync("data/assets/Cube.obj"));
    app.RunForever();
}
