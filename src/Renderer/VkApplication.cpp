#include <Renderer/VkApplication.hpp>
namespace Foundation {
    namespace Renderer {
        VkApplication::VkApplication(const char* appName, const char* engineName, const int appVersion, const int apiVersion) {
            auto vkAppInfo = vk::ApplicationInfo(
                appName,
                appVersion,
                engineName,
                s_RendererVersion,
                apiVersion
            );
            m_instance = vk::raii::Instance(m_context, vk::InstanceCreateInfo({}, &vkAppInfo));
        }
    }
}
