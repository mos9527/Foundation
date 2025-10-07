#include "App.hpp"
#include "Assets/Mesh.hpp"
#include <Math/Math.hpp>
#include <GLFW/glfw3.h>
#include <Bindings/ImGui.hpp>

using namespace Foundation;
using namespace Foundation::Core;
using namespace Foundation::RenderCore;
using namespace Foundation::Native;
using namespace Foundation::Math;
namespace ModelViewer
{
    /* -- States -- */
    SceneHandle mesh;    
    void App::OnDeviceSetup()
    {
        mGPUScene = ConstructUnique<GPUScene>(GetAllocator(), mDevice.Get(), GPUSceneBudgets{}, GetAllocator());
        mScene = ConstructUnique<Scene>(GetAllocator(), mGPUScene.get(), GetAllocator());
        /* -- ImGui -- */        
        ImGui_ImplFoundation_SetupContextWithDefaultStyles();
        ImGui_ImplFoundation_Init(
            static_cast<VulkanApplication*>(mRHI.get()),
            static_cast<VulkanDevice*>(mDevice.Get()), 
            static_cast<VulkanDeviceQueue*>(mDevice->GetDeviceQueue(RHIDeviceQueueType::Graphics)),
            mSwapchain.Get(), 
            reinterpret_cast<GLFWwindow*>(GetNativeWindow()->GetNative())
        );
        // TEST: Load mesh
        Vector<MeshVertex> vertices(GetAllocator());
        Vector<MeshIndex> indices(GetAllocator());
        meshLoadObjFile(vertices, indices, "data/assets/Suzanne.obj");
        SceneMeshData meshData = sceneMeshDataFromVertexIndex(vertices, indices, GetAllocator());
        mesh = mScene->PushMesh(meshData);
        mScene->mCamera.position = float3{2, 2, 10};
    }    
    void App::OnApplicationTick()
    {
        float time = GetApplicationTime();
        if (time > 1.0f)
            return;
        auto instances = mScene->MapInstances();
        for (int i = 0; i < 50 * 50; i++)
        {
            CHECK(i < instances.size());
            instances[i].meshAllocationRawOffsetPP = mScene->QueryMesh(mesh).selfRawOffset + 1;
            float theta = time * acos(-1) * 0.1f;
            instances[i].q = angleAxis(theta, float3{0, 0, 1}) * angleAxis(radians(90.0f), float3{1, 0, 0});
            instances[i].t = float3{ 2*(i / 50),  2*(i % 50), 0};
        }
        mScene->UnmapInstances();
    }    
    void App::OnImGui() {
        ImGui::Begin("Scene");
        ImGui::Text("FPS: %zu", mTiming.GetFPS());
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Camera"))
        {
            auto& cam = mScene->mCamera;
            ImGui::DragFloat3("Position", &cam.position.x, 0.1f);
            ImGui::DragFloat3("LookAt", &cam.lookAt.x, 0.1f);
            ImGui::DragFloat3("Up", &cam.up.x, 0.1f);
            ImGui::DragFloat("Vertical FOV (degrees)", &cam.verticalFov, 0.1f, radians(1.0f), radians(179.0f));
            ImGui::Text("Aspect Ratio: %.3f", cam.aspectRatio);
            ImGui::DragFloat("Near Plane", &cam.zNear, 1e-4f, 1e-4f, 100.f);
        }
        if (ImGui::CollapsingHeader("Controls"))
        {
            ImGui::Text("WASD - Move");
            ImGui::Text("Right Mouse + Drag - Look Around");
            ImGui::Text("Left Shift - Speed Boost");
        }
        ImGui::End();
    }
    void App::OnBeforeFrame() { 
        ImGui_ImplFoundation_OnBeforeFrame();
        OnImGui();
        mScene->mTime = GetApplicationTime();
        mScene->mCamera.aspectRatio = mSwapchain->GetAspectRatio();        
        static float t0 = 0; float t1 = GetApplicationTime(), dt = t1 - t0;
        /* -- Camera controls -- */
        GLFWwindow* win = reinterpret_cast<GLFWwindow*>(GetNativeWindow()->GetNative());
        // Movement
        auto& camera = mScene->mCamera;
        float3 view = normalize(camera.lookAt - camera.position);        
        float3 up = camera.up;
        float3 right = normalize(cross(view, up));
        float3 move{};
        float delta = (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 5.f : 2.f) * dt;
        if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS)
            move += view;
        if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS)
            move -= view;
        if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS)
            move -= right;
        if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS)
            move += right;
        if (length(move) > 0)
            move = normalize(move) * delta;
        camera.position += move; 
        camera.lookAt += move;
        // View
        static double lastX = 0, lastY = 0;
        double mX, mY; glfwGetCursorPos(win, &mX, &mY);
        double dX = mX - lastX, dY = mY - lastY;
        if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
        {
            float yaw = -static_cast<float>(dX) * 0.002f;
            float pitch = -static_cast<float>(dY) * 0.002f;
            quat qYaw = angleAxis(yaw, up);
            quat qPitch = angleAxis(pitch, right);
            quat q = normalize(qPitch * qYaw);
            view = q * view;
            camera.lookAt = camera.position + view * length(camera.lookAt - camera.position);
        }
        lastX = mX; lastY = mY;
        t0 = GetApplicationTime();
    }
    void App::OnAfterFrame() { ImGui_ImplFoundation_OnAfterFrame(); }
} // namespace ModelViewer
using namespace ModelViewer;
using namespace Foundation::Async;
int main(int argc, char** argv)
{
    App app;
    app.Initialize<VulkanApplication>({.windowTitle = "Model Viewer", .present = true, .asyncCompute = 0, .vsync = false});
    app.RunForever();
    ImGui_ImplFoundation_Shutdown();
}
