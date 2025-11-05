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
        ImGui_ImplFoundation_Init(mDevice.Get(), GetNativeWindow(), GetAllocator());
        // TEST: Load mesh
        Vector<MeshVertex> vertices(GetAllocator());
        Vector<MeshIndex> indices(GetAllocator());
        meshLoadObjFile(vertices, indices, "data/assets/Kitten.obj");
        MeshScratchBuffers meshData = sceneMeshDataFromVertexIndex(vertices, indices, GetAllocator());
        mesh = mScene->PushMesh(meshData);
        mScene->mCamera.position = float3{3,20,3};
        mScene->mCullingCamera = mScene->mCamera;
    }
    void App::OnRendererPostSetup()
    {
        CHECK(mGBufferSRV != kInvalidHandle);
        auto* srv = mRenderer->DerefTextureView(mGBufferSRV);
        if (mGBufferHandle)
            ImGui_ImplFoundation_RemoveImage(mGBufferHandle);
        mGBufferHandle = ImGui_ImplFoundation_AddImage(srv, ImGuiImplFoundationImageSamplerNearest);
    }
    void App::OnApplicationTick()
    {
        float time = GetApplicationTime();
        auto instances = mScene->MapInstances();
        constexpr int kCountSq = 1;
        mScene->mInstanceCount = kCountSq * kCountSq;
        for (size_t i = 0; i < mScene->mInstanceCount; i++)
        {
            constexpr float scale = 1.0f;
            CHECK(i < instances.size());
            instances[i].meshAllocationRawOffsetPP = mScene->QueryMesh(mesh).selfRawOffset + 1;
            float theta = time * acos(-1) * 0.1f;
            instances[i].q = quat(0,0,0,1); // angleAxis(theta, float3{0, 0, 1});
            instances[i].t = float3{scale * (i / kCountSq), scale * (i % kCountSq), sin(time + i)};
        }
        mScene->UnmapInstances();
    }
    void App::OnImGui()
    {
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (ImGui::Begin("Viewport"))
        {
            {
                ImVec2 size = ImGui::GetContentRegionAvail();
                mViewportSize.x = size.x, mViewportSize.y = size.y;
            }
            float2 size = mViewportSize, extent = {kTextureMaxExtent.x, kTextureMaxExtent.y};
            float2 uv1 = size / extent;
            ImGui::Image(mGBufferHandle, ImVec2(size.x, size.y), ImVec2(0, 0), ImVec2(uv1.x, uv1.y));
            ImGui::SetCursorPos(ImVec2{0, 0});
            drawGizmoCameraFrustum(mScene->mCamera.GetParams().viewProj,
                                   mScene->mCullingCamera.GetCullParams().viewProj);
        }
        ImGui::End();
        if (ImGui::Begin("Scene"))
        {
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
        }
        ImGui::End();
    }
    void App::OnBeforeFrame()
    {
        ImGui_ImplFoundation_NewFrame();
        ImGui::NewFrame();
        mScene->mTime = GetApplicationTime();
        mScene->mCamera.aspectRatio = mScene->mCullingCamera.aspectRatio =
            mViewportSize.x / static_cast<float>(mViewportSize.y);
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
        float3 up = float3{0,1,0};
        float3 forward = normalize(camera->orientation * float3{0,0,-1});
        float3 right = cross(forward, up);
        float3 move{};
        float delta = (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 5.f : 2.f) * dt;
        if (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS)
            move += forward;
        if (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS)
            move -= forward;
        if (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS)
            move -= right;
        if (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS)
            move += right;
        if (length(move) > 0)
            move = normalize(move) * delta;
        camera->position += move;
        // View
        static double lastX = 0, lastY = 0;
        double mX, mY;
        glfwGetCursorPos(win, &mX, &mY);
        double dX = mX - lastX, dY = mY - lastY;
        static float pitch = 0, yaw = -radians(90.0f);
        if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS)
        {
            pitch -= static_cast<float>(dX) * 0.002f;
            yaw += static_cast<float>(dY) * 0.002f;
        }
        quat qYaw = angleAxis(yaw, vec3{1,0,0});
        quat qPitch = angleAxis(pitch, vec3{0,1,0});
        camera->orientation = normalize(qYaw * qPitch);
        lastX = mX;
        lastY = mY;
        t0 = GetApplicationTime();
        static bool set = false;
        if (!set || glfwGetKey(win, GLFW_KEY_SPACE) == GLFW_PRESS)
        {
            set = true;
            mScene->mCullingCamera.position = mScene->mCamera.position;
            mScene->mCullingCamera.orientation = mScene->mCamera.orientation;
        }
        mScene->CommitParams();
        OnImGui();
    }

} // namespace ModelViewer
using namespace ModelViewer;
using namespace Foundation::Async;
int main(int, char**)
{
    App app;
    app.Initialize<VulkanApplication>({.windowTitle = "Model Viewer", .renderer = {.enableAsyncCompute=true}});
    app.RunForever();
    ImGui_ImplFoundation_Shutdown();
}
