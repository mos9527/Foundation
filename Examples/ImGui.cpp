#include <Bindings/ImGui.hpp>
#include "Examples.hpp"
namespace Examples
{
    /**
     * @example ImGui.cpp
     * ImGui integration example.
     */
    class ImGuiDemoApp : public RenderApplication
    {
        void OnDeviceSetup() override
        {
            ImGui_ImplFoundation_SetupContextWithDefaultStyles();
            ImGui_ImplFoundation_Init(mDevice.Get(), GetNativeWindow(), mAlloc.Ptr());
        }
        void OnRendererSetup() override
        {
            ImGui_ImplFoundation_CreatePass(mRenderer.get(), "ImGui");
        }
        void OnBeforeFrame() override
        {
            ImGui_ImplFoundation_NewFrame();
            ImGui::NewFrame();
            ImGui::ShowDemoWindow();
        }
    };

} // namespace Examples
int main(int argc, char** argv)
{
    Examples::ImGuiDemoApp app;
    app.Initialize<VulkanApplication>({.windowTitle = "ImGui", .vsync = true});
    app.RunForever();
    ImGui_ImplFoundation_Shutdown();
}
