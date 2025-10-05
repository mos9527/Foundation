#pragma once
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
#include <imgui.h>

#include <RHIVulkan/Application.hpp>
#include <RenderCore/Renderer.hpp>
#include <Core/Core.hpp>

struct GLFWwindow;
void ImGui_ImplFoundation_Init(Foundation::RHI::VulkanApplication* app, Foundation::RHI::VulkanDevice* device,
                        Foundation::RHI::VulkanDeviceQueue* queue, Foundation::RHI::RHISwapchain* swapchain,
                        GLFWwindow* window);
void ImGui_ImplFoundation_Shutdown();

void ImGui_ImplFoundation_OnBeforeFrame();
void ImGui_ImplFoundation_OnAfterFrame();

void* ImGui_ImplFoundation_CreatePass(Foundation::RenderCore::Renderer* renderer, Foundation::Core::StringView name = "ImGui");

void ImGui_ImplFoundation_SetupContextWithDefaultStyles();
