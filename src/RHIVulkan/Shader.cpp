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

void VulkanShaderModule::DebugSetObjectName(const char* name) {
    VkShaderModule handle = *m_shaderModule;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eShaderModule,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}
