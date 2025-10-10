#include "App.hpp"

#include <Gizmos/Gizmos.hpp>
#include <Math/Math.hpp>

#include <Bindings/ImGui.hpp>
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>

#include "Assets/Mesh.hpp"
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
        ImGui_ImplGlfw_InitForOther(static_cast<GLFWwindow*>(GetNativeWindow()->GetNative()), true);
        ImGui_ImplFoundation_Init(mDevice.Get(), GetAllocator());
        // TEST: Load mesh
        Vector<MeshVertex> vertices(GetAllocator());
        Vector<MeshIndex> indices(GetAllocator());
        meshLoadObjFile(vertices, indices, "data/assets/Kitten.obj");
        MeshScratchBuffers meshData = sceneMeshDataFromVertexIndex(vertices, indices, GetAllocator());
        mesh = mScene->PushMesh(meshData);
        mScene->mCamera.position = float3{2, 2, 10};
        mScene->mCullingCamera = mScene->mCamera;
    }
    void App::OnApplicationTick()
    {
        float time = GetApplicationTime();
        if (time > 10) return;
        auto instances = mScene->MapInstances();
        constexpr int countSq = 10;
        constexpr float scale = 1.0f;
        mScene->mInstanceCount = countSq * countSq;
        for (size_t i = 0; i < mScene->mInstanceCount; i++)
        {
            CHECK(i < instances.size());
            instances[i].meshAllocationRawOffsetPP = mScene->QueryMesh(mesh).selfRawOffset + 1;
            float theta = time * acos(-1) * 0.1f;
            instances[i].q = angleAxis(theta, float3{0, 0, 1}) * angleAxis(radians(90.0f), float3{1, 0, 0});
            instances[i].t = float3{scale * (i / countSq), scale * (i % countSq), sin(time + i)};
        }
        mScene->UnmapInstances();
    }
    void App::OnImGui()
    {
        ImGui::Begin("Scene");
        ImGui::Text("FPS: %zu", mTiming.GetFPS());
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Camera"))
        {
            mScene->mCamera.OnImGui();
        }
        if (ImGui::CollapsingHeader("Culling Camera"))
        {
            mScene->mCullingCamera.OnImGui();
        }
        if (ImGui::CollapsingHeader("Grid"))
        {
            mScene->mGrid.OnImGui();
        }
        if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("WASD - Move");
            ImGui::Text("Right Mouse + Drag - Look Around");
            ImGui::Text("Left Shift - Speed Boost");
            ImGui::Text("Left Alt - Control Culling Camera");
            ImGui::Text("Space - Set Culling Camera to Main Camera");
        }
        ImGui::End();
    }
    void App::OnBeforeFrame() {
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        mScene->mTime = GetApplicationTime();
        mScene->mCamera.aspectRatio = mSwapchain->GetAspectRatio();
        mScene->mCullingCamera.aspectRatio = mSwapchain->GetAspectRatio();
        static float t0 = 0;
        float t1 = GetApplicationTime(), dt = t1 - t0;
        /* -- Camera controls -- */
        GLFWwindow* win = static_cast<GLFWwindow*>(GetNativeWindow()->GetNative());
        // Movement
        Camera* camera = &mScene->mCamera;
        if (glfwGetKey(win, GLFW_KEY_LEFT_ALT) == GLFW_PRESS)
        {
            camera = &mScene->mCullingCamera;
        }
        float3 view = normalize(camera->lookAt - camera->position);
        float3 up = camera->up;
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
        camera->position += move;
        camera->lookAt += move;
        // View
        static double lastX = 0, lastY = 0;
        double mX, mY;
        glfwGetCursorPos(win, &mX, &mY);
        double dX = mX - lastX, dY = mY - lastY;
        if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
        {
            float yaw = -static_cast<float>(dX) * 0.002f;
            float pitch = -static_cast<float>(dY) * 0.002f;
            quat qYaw = angleAxis(yaw, up);
            quat qPitch = angleAxis(pitch, right);
            quat q = normalize(qPitch * qYaw);
            view = q * view;
            camera->lookAt = camera->position + view * length(camera->lookAt - camera->position);
        }
        lastX = mX;
        lastY = mY;
        t0 = GetApplicationTime();
        if (glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS)
            mScene->mCullingCamera = mScene->mCamera;
        mScene->CommitParams();
        gizmosDrawCameraFrustum(mScene->mCamera.GetParams().viewProj, mScene->mCullingCamera.GetCullParams().viewProj);
        OnImGui();
    }
    void App::OnAfterFrame() { /* nop */ }
} // namespace ModelViewer
using namespace ModelViewer;
using namespace Foundation::Async;
int main(int argc, char** argv)
{
    App app;
    app.Initialize<VulkanApplication>(
        {.windowTitle = "Model Viewer", .present = true, .asyncCompute = true, .vsync = false /* !! */});
    app.RunForever();
    ImGui_ImplFoundation_Shutdown();
}
