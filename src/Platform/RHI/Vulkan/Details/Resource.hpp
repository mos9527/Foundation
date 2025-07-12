#pragma once
#include <Platform/RHI/Details/Resource.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            inline vk::Format GetVulkanFormatFromRHIFormat(RHIResourceFormat format) {
                using enum RHIResourceFormat;
                switch (format) {
                case R8G8B8A8_UNORM: return vk::Format::eR8G8B8A8Unorm;
                default:
                    return vk::Format::eUndefined;
                }
            }
        }
    }
}
