using namespace Foundation::RHI;
VulkanShaderModule::VulkanShaderModule(const VulkanDevice& device, ShaderModuleDesc const& desc)
    : RHIShaderModule(device, desc), mDevice(device) {
    vk::ShaderModuleCreateInfo create_info{
        .codeSize = desc.source.size(),
        .pCode = reinterpret_cast<const uint32_t*>(desc.source.data())
    };
    mShaderModule = vk::raii::ShaderModule(device.GetVkDevice(), create_info, mDevice.GetVkAllocatorCallbacks());
}

void VulkanShaderModule::DebugSetObjectName(const char* name) {
    VkShaderModule handle = *mShaderModule;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::eShaderModule,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
        });
}
