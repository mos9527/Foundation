#pragma once
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

namespace Foundation {
    namespace Renderer {
        constexpr int s_RendererVersion = VK_MAKE_VERSION(
            FOUNDATION_RENDERER_VERSION_MAJOR,
            FOUNDATION_RENDERER_VERSION_MINOR,
            FOUNDATION_RENDERER_VERSION_PATCH
        );
        class VkApplication {
            vk::raii::Context m_context;
            vk::raii::Instance m_instance{ nullptr };
        public:
            VkApplication(const char* appName, const char* engineName, const int appVersion, const int apiVersion);
        };
    }
}

