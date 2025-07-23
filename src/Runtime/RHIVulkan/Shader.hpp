#pragma once
#include <RHICore/Shader.hpp>
#include "Common.hpp"
namespace Foundation::RHI {
    class VulkanDevice;
    class VulkanShaderModule : public RHIShaderModule {
        const VulkanDevice& m_device;
        vk::raii::ShaderModule m_shaderModule{ nullptr };
    public:
        VulkanShaderModule(const VulkanDevice& device, ShaderModuleDesc const& desc);

        inline const vk::raii::ShaderModule& GetVkShaderModule() const { return m_shaderModule; }
    };
}  
