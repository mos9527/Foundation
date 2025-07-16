#pragma once
#include <Platform/RHI/Common.hpp>

namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIDevice;
            class RHIShaderModule : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                struct ShaderModuleDesc {
                    std::string name;
                    Core::StlSpan<char> source;
                };
                const ShaderModuleDesc m_desc;

                RHIShaderModule(RHIDevice const& device, ShaderModuleDesc const& desc) : m_device(device), m_desc(desc) {}
            };
        }
    }
}
