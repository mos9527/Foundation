#include <Platform/RHI/Vulkan/Shader.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>

using namespace Foundation::Platform::RHI;
VulkanShaderModule::VulkanShaderModule(const VulkanDevice& device, ShaderModuleDesc const& desc)
    : RHIShaderModule(device, desc), m_device(device) {
    vk::ShaderModuleCreateInfo create_info{
        .codeSize = desc.source.size(),
        .pCode = reinterpret_cast<const uint32_t*>(desc.source.data())
    };
    m_shaderModule = vk::raii::ShaderModule(device.GetVkDevice(), create_info);
}

VulkanShaderPipelineModule::VulkanShaderPipelineModule(const VulkanDevice& device, ShaderPipelineModuleDesc const& desc, RHIDeviceObjectHandle<RHIShaderModule> shader_module)
    : RHIShaderPipelineModule(device, desc, shader_module), m_device(device) {
    if (!m_shader_module.IsFrom(&device))
        throw std::runtime_error("shader module isn't created by the same device");
}

vk::PipelineShaderStageCreateInfo VulkanShaderPipelineModule::GetVkPipelineShaderStageCreateInfo() {
    return vk::PipelineShaderStageCreateInfo{        
        .stage = GetVulkanShaderStageFromDesc(m_desc.stage),
        .module = m_shader_module.Get<VulkanShaderModule>()->GetVkShaderModule(),
        .pName = m_desc.entry_point.c_str(),
        .pSpecializationInfo = nullptr // TODO: Handle specialization info
    };
}
