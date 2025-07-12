#pragma once
#include <Platform/RHI/Shader.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            inline vk::ShaderStageFlagBits GetVulkanShaderStageFromDesc(RHIShaderPipelineModule::ShaderPipelineModuleDesc::StageType stage) {
                using enum RHIShaderPipelineModule::ShaderPipelineModuleDesc::StageType;
                switch (stage) {
                case FRAGMENT: return vk::ShaderStageFlagBits::eFragment;
                case COMPUTE: return vk::ShaderStageFlagBits::eCompute;
                case VERTEX:
                default:                    
                    return vk::ShaderStageFlagBits::eVertex;
                }
            }

            class VulkanDevice;
            class VulkanShaderModule : public RHIShaderModule {
                const VulkanDevice& m_device;
                vk::raii::ShaderModule m_shaderModule{ nullptr };                
            public:
                VulkanShaderModule(const VulkanDevice& device, ShaderModuleDesc const& desc);

                inline bool IsValid() const { return m_shaderModule != nullptr; }
                inline const char* GetName() const { return m_desc.name.c_str(); }

                inline const vk::raii::ShaderModule& GetVkShaderModule() const { return m_shaderModule; }
            };

            class VulkanShaderPipelineModule : public RHIShaderPipelineModule {
                const VulkanDevice& m_device;
            public:
                VulkanShaderPipelineModule(const VulkanDevice& device, ShaderPipelineModuleDesc const& desc, RHIDeviceObjectHandle<RHIShaderModule> shader_module);
                vk::PipelineShaderStageCreateInfo GetVkPipelineShaderStageCreateInfo();

                inline bool IsValid() const { return m_shader_module.IsValid(); }
                inline const char* GetName() const { return IsValid() ? m_shader_module->GetName() : nullptr; }

            };
        }
    }
};
