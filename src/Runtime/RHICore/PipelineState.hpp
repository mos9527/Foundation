#pragma once
#include "Common.hpp"
#include "Shader.hpp"
#include "Descriptor.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    class RHIPipelineState : public RHIObject {
    protected:
        const RHIDevice& m_device;
    public:
        struct PipelineStateDesc {
            RHIDevicePipelineType type{ RHIDevicePipelineType::Graphics };
            // [Graphics] Vertex Input
            struct VertexInput {
                struct Binding {
                    uint32_t stride; // In bytes
                    bool per_instance{ false }; // If true, this binding is per-instance data
                };
                Core::Span<const Binding> bindings;
                Core::Span<const RHIVertexAttribute> attributes;
            } vertex_input{};
            // [Graphics] Input Assembly
            enum Topology {
                LINE_LIST,
                POINT_LIST,
                TRIANGLE_LIST,
                TRIANGLE_STRIP
            } topology{ TRIANGLE_LIST };
            // [Graphics] Viewport
            struct Viewport {
                float x = 0, y = 0, width, height;
                float min_depth = 0.0, max_depth = 1.0;
            } viewport{};
            // [Graphics] Scissor
            struct Scissor {
                int32_t x = 0, y = 0, width, height;
            } scissor{};
            // [Graphics] Rasterizer
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
            // [Graphics] MSAA
            struct Multisample {
                bool enabled;
                RHIMultisampleCount sample_count; // 1, 2, 4, 8, etc.
            } multisample{};
            // [Graphics] Depth Stencil
            struct DepthStencil {
                RHIResourceFormat depth_format{ RHIResourceFormat::Undefined };
                RHIResourceFormat stencil_format{ RHIResourceFormat::Undefined };
                bool depth_test{ false };
                bool depth_write{ false };
                enum CompareOp {
                    NEVER,
                    LESS,
                    EQUAL,
                    LESS_EQUAL,
                    GREATER,
                    NOT_EQUAL,
                    GREATER_EQUAL,
                    ALWAYS
                } depth_compare_op{ LESS };
            } depth_stencil{};
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
            // [Graphics] Attachments/Alpha Blending
            Core::Span<const Attachment> attachments;
            // Stages
            struct ShaderStage {
                struct StageDesc {
                    // Stage this shader participates in
                    // You can only specify one stage per shader module.
                    RHIShaderStage stage;
                    const char* entry_point;
                    struct SpecializationInfo {
                        // !! TODO
                    } specialization_info;
                } desc;
                RHIDeviceObjectHandle<RHIShaderModule> shader_module;
            };
            Core::Span<const ShaderStage> shader_stages;
            // Descriptors
            Core::Span<const RHIDeviceObjectHandle<RHIDeviceDescriptorSetLayout>> descriptor_set_layouts;
            // Push Constants
            struct PushConstant {
                RHIShaderStage stage;
                size_t offset;
                size_t size;
            };
            Core::Span<const PushConstant> push_constants;
        };
        const PipelineStateDesc m_desc;

        RHIPipelineState(RHIDevice const& device, PipelineStateDesc const& desc) : m_device(device), m_desc(desc) {}

        virtual void DebugSetObjectName(const char* name) = 0;
    };
}
