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

const Native::Path kDefaultFontPath = "./data/assets/LXGWNeoXiHei.ttf";
void ImGui_ImplFoundation_SetupContextWithDefaultStyles()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking
    io.ConfigDpiScaleFonts = true; // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor
                                   // DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
    // Styles from
    // https://github.com/KhronosGroup/Vulkan-Samples/blob/b9961792604af2ede4c9d0868947de2a8eccd549/framework/gui.h#L338
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.005f, 0.005f, 0.005f, 0.94f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.6f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_Header] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.8f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.1f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.2f);
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 0.0f, 0.0f, 0.6f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);
    // Font from https://github.com/lxgw/LxgwNeoXiHei
    if (!std::filesystem::exists(kDefaultFontPath))    
        LOG_RUNTIME(ImGui, err, "Font file {} not found! ImGui will use default font.", kDefaultFontPath);
    else
    {
        io.Fonts->Clear();
        ImFontConfig font_cfg;
        io.Fonts->AddFontFromFileTTF(kDefaultFontPath.string().c_str(), 16.0f, &font_cfg);
    }
}