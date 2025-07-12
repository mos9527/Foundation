#pragma once
#include <Platform/RHI/Details/Details.hpp>
#include <Platform/RHI/Details/Resource.hpp>
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
                    // !! TODO
                    // Input Assembly
                    enum Topology {
                        LINE_LIST,
                        POINT_LIST,
                        TRIANGLE_LIST,
                        TRIANGLE_STRIP
                    } topology;
                    // Viewport
                    struct Viewport {
                        float x, y, width, height;
                        float min_depth, max_depth;
                    } viewport;
                    // Scissor
                    struct Scissor {
                        int32_t x, y, width, height;
                    } scissor;
                    // Rasterizer
                    struct Rasterizer {
                        enum FillMode {
                            FILL_WIREFRAME,
                            FILL_SOLID
                        } fill_mode;
                        enum CullMode {
                            CULL_NONE,
                            CULL_FRONT,
                            CULL_BACK
                        } cull_mode;
                        enum FrontFace {
                            FF_COUNTER_CLOCKWISE,
                            FF_CLOCKWISE
                        } front_face;
                        bool enable_depth_bias;
                        float depth_bias;
                        float line_fill_width;
                    } rasterizer;
                    // MSAA
                    struct Multisample {
                        bool enabled;
                        enum Count {
                            e1, e2, e4, e8, e16
                        } sample_count; // 1, 2, 4, 8, etc.
                    } multisample;
                    // Depth Stencil
                    // !! TODO
                    // Attachments/Alpha Blending
                    struct Attachment {
                        struct Blending {
                            bool enabled;
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
                            float blend_constant[4]; // RGBA
                        } blending;
                        struct RenderTarget {
                            RHIResourceFormat format;
                        } render_target;
                    };
                    std::vector<Attachment> attachments;
                    // Stages
                    std::vector<RHIDeviceObjectHandle<RHIShaderPipelineModule>> shader_stages;
                };
                const PipelineStateDesc m_desc;

                RHIPipelineState(RHIDevice const& device, PipelineStateDesc const& desc) : m_device(device), m_desc(desc) {}
            };
        }
    }
}
