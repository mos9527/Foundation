#pragma once
#include "Common.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    class RHIShaderModule : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        struct ShaderModuleDesc {
            Span<char> source;
        };
        const ShaderModuleDesc mDesc;

        RHIShaderModule(RHIDevice const& device, ShaderModuleDesc const& desc) : mDevice(device), mDesc(desc) {}

        virtual void DebugSetObjectName(const char* name) = 0;
    };
}
