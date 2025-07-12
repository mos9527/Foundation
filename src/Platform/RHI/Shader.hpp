#pragma once
#include <Platform/RHI/Details/Details.hpp>

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
                    std::string source;
                };
                const ShaderModuleDesc m_desc;

                RHIShaderModule(RHIDevice const& device, ShaderModuleDesc const& desc) : m_device(device), m_desc(desc) {}
            };
            class RHIShaderPipelineModule : public RHIObject {
            protected:
                const RHIDevice& m_device;
                RHIDeviceObjectHandle<RHIShaderModule> m_shader_module;
            public:
                struct ShaderPipelineModuleDesc {
                    enum StageType {
                        VERTEX,
                        FRAGMENT,
                        COMPUTE
                    } stage;
                    std::string entry_point;
                    struct SpeicalizationInfo {
                        // !! TODO
                    } specialization_info;
                };
                const ShaderPipelineModuleDesc m_desc;
                inline auto GetStage() const { return m_desc.stage; }

                RHIShaderPipelineModule(RHIDevice const& device, ShaderPipelineModuleDesc const& desc, RHIDeviceObjectHandle<RHIShaderModule> shader_module)
                    : m_device(device), m_desc(desc), m_shader_module(shader_module) {
                }
            };
        }
    }
}
