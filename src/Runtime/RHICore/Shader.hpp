#pragma once
#include "Common.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    class RHIShaderModule : public RHIObject {
    protected:
        const RHIDevice& m_device;
    public:
        struct ShaderModuleDesc {
            Core::StlSpan<char> source;
        };
        const ShaderModuleDesc m_desc;

        RHIShaderModule(RHIDevice const& device, ShaderModuleDesc const& desc) : m_device(device), m_desc(desc) {}
    };
}
