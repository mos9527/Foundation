#pragma once
#include <Platform/RHI/Common.hpp>
#include <Platform/RHI/Shader.hpp>

namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIDevice;
            class RHIPipelineState : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                struct PipelineStateDesc {
                    std::string name;
                    // Vertex Input
                    struct VertexInput {
                        struct Binding {
                            uint32_t stride; // In bytes
                            bool per_instance{ false }; // If true, this binding is per-instance data
                        };
                        Core::StlSpan<const Binding> bindings;
                        struct Attribute {
                            uint32_t location; // Index into shader input
                            uint32_t binding; // 0-indexed index into bindings.
                            uint32_t offset; // In bytes
                            RHIResourceFormat format{ RHIResourceFormat::Undefined }; // Format on the GPU    
                        };                        
                        Core::StlSpan<const Attribute> attributes;
                    } vertex_input{};
                    // Input Assembly
                    enum Topology {
                        LINE_LIST,
                        POINT_LIST,
                        TRIANGLE_LIST,
                        TRIANGLE_STRIP
                    } topology{ TRIANGLE_LIST };
                    // Viewport
                    struct Viewport {
                        float x = 0, y = 0, width, height;
                        float min_depth = 0.0, max_depth = 1.0;
                    } viewport{};
                    // Scissor
                    struct Scissor {
                        int32_t x = 0, y = 0, width, height;
                    } scissor{};
                    // Rasterizer
                    struct Rasterizer {
                        enum FillMode {
                            FILL_WIREFRAME,
                            FILL_SOLID
                        } fill_mode{ FILL_SOLID };
                        enum CullMode {
                            CULL_NONE,
                            CULL_FRONT,
                            CULL_BACK
                        } cull_mode{ CULL_BACK };
                        enum FrontFace {
                            FF_COUNTER_CLOCKWISE,
                            FF_CLOCKWISE
                        } front_face{ FF_CLOCKWISE };
                        bool enable_depth_bias{ false };
                        float depth_bias = 1.0;
                        float line_fill_width = 1.0;
                    } rasterizer{};
                    // MSAA
                    struct Multisample {
                        bool enabled;
                        enum Count {
                            e1, e2, e4, e8, e16
                        } sample_count; // 1, 2, 4, 8, etc.
                    } multisample{};
                    // Depth Stencil
                    // !! TODO
                    // Attachments/Alpha Blending
                    struct Attachment {
                        struct Blending {
                            bool enabled{ false };
                            enum BlendFactor {
                                ZERO,
                                ONE,
                                SRC_COLOR,
                                ONE_MINUS_SRC_COLOR,
                                DST_COLOR,
                                ONE_MINUS_DST_COLOR,
                                SRC_ALPHA,
                                ONE_MINUS_SRC_ALPHA,
                                DST_ALPHA,
                                ONE_MINUS_DST_ALPHA
                            } src_color_blend_factor, dst_color_blend_factor;
                            enum BlendOp {
                                ADD,
                                SUBTRACT,
                                REVERSE_SUBTRACT
                            } color_blend_op;
                            float blend_constant[4]{}; // RGBA
                        } blending;
                        struct RenderTarget {
                            RHIResourceFormat format{ RHIResourceFormat::Undefined };
                        } render_target{};
                    };
                    Core::StlSpan<const Attachment> attachments;
                    // Stages
                    struct ShaderStage {
                        struct StageDesc {
                            enum StageType {
                                VERTEX,
                                FRAGMENT,
                                COMPUTE
                            } stage;
                            std::string entry_point;
                            struct SpecializationInfo {
                                // !! TODO
                            } specialization_info;
                        } desc;
                        RHIDeviceObjectHandle<RHIShaderModule> shader_module;
                    };
                    Core::StlSpan<const ShaderStage> shader_stages;
                };
                const PipelineStateDesc m_desc;

                RHIPipelineState(RHIDevice const& device, PipelineStateDesc const& desc) : m_device(device), m_desc(desc) {}
            };
        }
    }
}
