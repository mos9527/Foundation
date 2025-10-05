#include "ImGui.hpp"
#include <GLFW/glfw3.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <RHIVulkan/Command.hpp>
using namespace Foundation;
using namespace Core;
using namespace RHI;
using namespace RenderCore;
constexpr size_t kImGuiVulkanDescriptorPoolSize = 1000;
void ImGui_ImplFoundation_Init(VulkanApplication* app, VulkanDevice* device, VulkanDeviceQueue* queue,
                                     RHISwapchain* swapchain, GLFWwindow* window)
{
    IMGUI_CHECKVERSION();
    // Setup Dear ImGui context    
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport / Platform Windows
    // io.ConfigViewportsNoAutoMerge = true;
    // io.ConfigViewportsNoTaskBarIcon = true;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    // ImGui::StyleColorsLight();

    // Setup scaling
    ImGuiStyle& style = ImGui::GetStyle();
    io.ConfigDpiScaleFonts = true; // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor
                                   // DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
    io.ConfigDpiScaleViewports = true; // [Experimental] Scale Dear ImGui and Platform Windows when Monitor DPI changes.

    // When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular
    // ones.
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo v{};
    v.ApiVersion = VK_API_VERSION_1_3;
    v.Instance = *app->GetVkInstance();
    v.PhysicalDevice = *device->GetVkPhysicalDevice();
    v.Device = *device->GetVkDevice();
    v.QueueFamily = queue->GetVkQueueIndex();
    v.Queue = *queue->GetVkQueue();
    v.DescriptorPoolSize = kImGuiVulkanDescriptorPoolSize;
    v.MinImageCount = 3;
    v.ImageCount = swapchain->GetImages().size();
    v.UseDynamicRendering = true;
    vk::Format swapchainFormat = vkFormatFromRHIFormat(swapchain->mDesc.format);
    v.PipelineRenderingCreateInfo = vk::PipelineRenderingCreateInfo{
        .colorAttachmentCount = 1, .pColorAttachmentFormats = &swapchainFormat
    };
    v.Allocator = app->GetVkAllocatorCallbacks();
    ImGui_ImplVulkan_Init(&v);
}

void ImGui_ImplFoundation_Shutdown()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void ImGui_ImplFoundation_OnBeforeFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGui_ImplFoundation_OnAfterFrame()
{    
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

void* ImGui_ImplFoundation_CreatePass(Renderer* renderer, StringView name)
{
    return createPass(renderer, name, RHIDeviceQueueType::Graphics,
        [](PassHandle self, Renderer* r) {
            r->BindBackbufferRTV(self);
        },
        [](PassHandle self, Renderer* r, RHI::RHICommandList* cmd) {
            auto const& img_wh = r->GetSwapchainExtent();         
            
            r->CmdBeginGraphics(self, cmd, img_wh, {} /* no RTV clear */);
            ImGui::Render();
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                            *static_cast<VulkanCommandList*>(cmd)->GetVkCommandBuffer());            
            cmd->EndGraphics();
        });
}
