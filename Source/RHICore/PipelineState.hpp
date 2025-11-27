#pragma once
#include "Common.hpp"
#include "Shader.hpp"
#include "Descriptor.hpp"
namespace Foundation::RHI {
    class RHIDevice;
    class RHIPipelineStateCache : public RHIObject
    {
    protected:
        const RHIDevice& mDevice;
    public:
        struct PipelineStateCacheDesc
        {
            Span<const char> initialData{};
        };
        const PipelineStateCacheDesc mDesc;
        RHIPipelineStateCache(RHIDevice const& device, PipelineStateCacheDesc const& desc) : mDevice(device), mDesc(desc) {}

        [[nodiscard]] virtual size_t GetCachedData(void* dstBuffer = nullptr) const = 0;
        virtual void DebugSetObjectName(const char* name) = 0;
    };
    class RHIPipelineState : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        struct PipelineStateDesc {
            RHIPipelineStateCache* psoCache{nullptr};
            RHIDevicePipelineType type{ RHIDevicePipelineType::Graphics };
            // [Graphics] Vertex Input
            struct VertexInput {
                struct Binding {
                    uint32_t stride; // In bytes
                    bool perInstance{ false }; // If true, this binding is per-instance data
                };
                Span<const Binding> bindings;
                Span<const RHIVertexAttribute> attributes;
            } vertexInput{};
            // [Graphics] Input Assembly
            enum Topology {
                LineList,
                PointList,
                TriangleList,
                TriangleStrip
            } topology{ TriangleList };
            // [Graphics] Viewport
            struct Viewport {
                float x = 0, y = 0, width, height;
                float minDepth = 0.0, maxDepth = 1.0;
            } viewport{};
            // [Graphics] Scissor
            struct Scissor {
                int32_t x = 0, y = 0, width, height;
            } scissor{};
            // [Graphics] Rasterizer
            struct Rasterizer {
                enum FillMode {
                    FillWireframe,
                    FillSolid
                } fillMode{ FillSolid };
                enum CullMode {
                    CullNone,
                    CullFront,
                    CullBack
                } cullMode{ CullBack };
                enum FrontFace {
                    FFCounterClockwise,
                    FFClockwise
                } frontFace{ FFCounterClockwise };
                bool enableDepthBias{ false };
                float depthBias = 1.0;
                float lineFillWidth = 1.0;
            } rasterizer{};
            // [Graphics] MSAA
            struct Multisample {
                bool enabled;
                RHIMultisampleCount sampleCount; // 1, 2, 4, 8, etc.
            } multisample{};
            // [Graphics] Depth Stencil
            struct DepthStencil {
                RHIResourceFormat depthFormat{ RHIResourceFormat::Undefined };
                RHIResourceFormat stencilFormat{ RHIResourceFormat::Undefined };
                bool depthTest{ true };
                bool depthWrite{ true };
                enum CompareOp {
                    Never,
                    Less,
                    Equal,
                    LessEqual,
                    Greater,
                    NotEqual,
                    GreaterEqual,
                    Always
                } depthCompareOp{ Greater };
            } depthStencil{};
            struct Attachment {
                struct Blending {
                    bool enabled{ false };
                    enum BlendFactor {
                        Zero,
                        One,
                        SrcColor,
                        OneMinusSrcColor,
                        DstColor,
                        OneMinusDstColor,
                        SrcAlpha,
                        OneMinusSrcAlpha,
                        DstAlpha,
                        OneMinusDstAlpha
                    } srcColorBlendFactor, dstColorBlendFactor, srcAlphaBlendFactor, dstAlphaBlendFactor;
                    enum BlendOp {
                        Add,
                        Subtract,
                        ReverseSubtract
                    } colorBlendOp, alphaBlendOp;
                    const static Blending GetNoBlending() { return {}; }
                    const static Blending GetAlphaBlending() {
                        return {
                            .enabled = true,
                            .srcColorBlendFactor = SrcAlpha,
                            .dstColorBlendFactor = OneMinusSrcAlpha,                            
                            .srcAlphaBlendFactor = One,
                            .dstAlphaBlendFactor = OneMinusSrcAlpha,
                            .colorBlendOp = Add,
                            .alphaBlendOp = Add
                        };
                    }
                    const static Blending GetAdditiveBlending()
                    {
                        return {
                            .enabled = true,
                            .srcColorBlendFactor = SrcAlpha,
                            .dstColorBlendFactor = One,                            
                            .srcAlphaBlendFactor = One,
                            .dstAlphaBlendFactor = One,
                            .colorBlendOp = Add,
                            .alphaBlendOp = Add
                        };
                    }
                } blending;
                struct RenderTarget {
                    RHIResourceFormat format{ RHIResourceFormat::Undefined };
                } renderTarget{};
            };
            // [Graphics] Attachments/Alpha Blending
            Span<const Attachment> attachments;
            // Stages
            struct ShaderStage {
                struct StageDesc {
                    // Stage this shader participates in
                    // You can only specify one stage per shader module.
                    RHIShaderStage stage;
                    const char* entryPoint;
                    // Only one specialization info per stage for simplicity
                    Span<const char> specializationData{};
                } desc;
                RHIShaderModule* shaderModule;
            };
            Span<const ShaderStage> shaderStages;
            // Descriptors
            Span<RHIDeviceDescriptorSetLayout* const> descriptorSetLayouts;
            // Push Constants
            struct PushConstant {
                RHIShaderStage stage;
                size_t offset;
                size_t size;
            };
            Span<const PushConstant> pushConstants;
        };
        const PipelineStateDesc mDesc;

        RHIPipelineState(RHIDevice const& device, PipelineStateDesc const& desc) : mDevice(device), mDesc(desc) {}

        virtual void DebugSetObjectName(const char* name) = 0;
    };

}
