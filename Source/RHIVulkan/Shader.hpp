#pragma once
#include <RHICore/Shader.hpp>
#include "Common.hpp"
namespace Foundation::RHI {
    class VulkanDevice;
    class VulkanShaderModule : public RHIShaderModule {
        const VulkanDevice& mDevice;
        vk::raii::ShaderModule mShaderModule{ nullptr };
    public:
        VulkanShaderModule(const VulkanDevice& device, ShaderModuleDesc const& desc);

        [[nodiscard]] inline const vk::raii::ShaderModule& GetVkShaderModule() const { return mShaderModule; }

        void DebugSetObjectName(const char* name) override;
    };
}  
