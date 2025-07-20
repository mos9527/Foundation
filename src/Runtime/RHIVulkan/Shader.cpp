#include "Shader.hpp"
#include "Device.hpp"

using namespace Foundation::RHI;
VulkanShaderModule::VulkanShaderModule(const VulkanDevice& device, ShaderModuleDesc const& desc)
    : RHIShaderModule(device, desc), m_device(device) {
    vk::ShaderModuleCreateInfo create_info{
        .codeSize = desc.source.size(),
        .pCode = reinterpret_cast<const uint32_t*>(desc.source.data())
    };
    m_shaderModule = vk::raii::ShaderModule(device.GetVkDevice(), create_info, m_device.GetVkAllocatorCallbacks());
}
