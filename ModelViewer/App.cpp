#include "App.hpp"
#include "Assets/Mesh.hpp"
#include <Math/Math.hpp>
#include <GLFW/glfw3.h>
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
        // TEST: Load mesh
        Vector<MeshVertex> vertices(GetAllocator());
        Vector<MeshIndex> indices(GetAllocator());
        meshLoadObjFile(vertices, indices, "data/assets/Kitten.obj");
        SceneMeshData meshData = sceneMeshDataFromVertexIndex(vertices, indices, GetAllocator());
        mesh = mScene->PushMesh(meshData);
        mScene->mCamera.position = float3{2, 2, 2};
    }    
    void App::OnApplicationTick()
    {
        auto instances = mScene->MapInstances();
        instances[0].meshAllocationRawOffsetPP = mScene->QueryMesh(mesh).selfRawOffset + 1;
        float time = GetApplicationTime();
        float theta = time * acos(-1) * 0.1f;
        instances[0].q = angleAxis(theta, float3{0, 0, 1}) * angleAxis(radians(90.0f), float3{1, 0, 0});
        mScene->UnmapInstances();
    }    
    void App::OnBeforeFrame() { 
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
            move -= right, camera.lookAt -= right * delta;
        if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS)
            move += right, camera.lookAt += right * delta;
        if (length(move) > 0)
            move = normalize(move) * delta;
        camera.position += move; 
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
} // namespace ModelViewer
using namespace ModelViewer;
using namespace Foundation::Async;
int main(int argc, char** argv)
{
    App app;
    app.Initialize<VulkanApplication>({.windowTitle = "Model Viewer", .present = true, .asyncCompute = true, .vsync = false});
    app.RunForever();
}
