#pragma once
#include <RHICore/PipelineState.hpp>
#include "Common.hpp"
namespace Foundation::RHI {
    inline vk::PrimitiveTopology GetVulkanPrimitiveTopologyFromDesc(RHIPipelineState::PipelineStateDesc::Topology topology) {
        using enum RHIPipelineState::PipelineStateDesc::Topology;
        switch (topology) {
        case PointList: return vk::PrimitiveTopology::ePointList;
        case LineList: return vk::PrimitiveTopology::eLineList;
        case TriangleStrip: return vk::PrimitiveTopology::eTriangleStrip;
        case TriangleList:
        default:
            return vk::PrimitiveTopology::eTriangleList;
        }
    }
    inline vk::PolygonMode GetVulkanPolygonModeFromDesc(RHIPipelineState::PipelineStateDesc::Rasterizer::FillMode mode) {
        switch (mode) {
        case RHIPipelineState::PipelineStateDesc::Rasterizer::FillWireframe: return vk::PolygonMode::eLine;
        case RHIPipelineState::PipelineStateDesc::Rasterizer::FillSolid: return vk::PolygonMode::eFill;
        default:
            return vk::PolygonMode::ePoint;
        }
    }

    inline vk::CullModeFlags GetVulkanCullModeFromDesc(RHIPipelineState::PipelineStateDesc::Rasterizer::CullMode mode) {
        switch (mode) {
        case RHIPipelineState::PipelineStateDesc::Rasterizer::CullNone: return vk::CullModeFlagBits::eNone;
        case RHIPipelineState::PipelineStateDesc::Rasterizer::CullFront: return vk::CullModeFlagBits::eFront;
        case RHIPipelineState::PipelineStateDesc::Rasterizer::CullBack:
        default:
            return vk::CullModeFlagBits::eBack;
        }
    }

    inline vk::FrontFace GetVulkanFrontFaceFromDesc(RHIPipelineState::PipelineStateDesc::Rasterizer::FrontFace face) {
        switch (face) {
        case RHIPipelineState::PipelineStateDesc::Rasterizer::FfCounterClockwise: return vk::FrontFace::eCounterClockwise;
        case RHIPipelineState::PipelineStateDesc::Rasterizer::FfClockwise:
        default:
            return vk::FrontFace::eClockwise;
        }
    }

    inline vk::BlendFactor GetVulkanBlendFactorFromDesc(RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendFactor factor) {
        using enum RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendFactor;
        switch (factor) {
        case ZERO: return vk::BlendFactor::eZero;
        case ONE: return vk::BlendFactor::eOne;
        case SrcColor: return vk::BlendFactor::eSrcColor;
        case OneMinusSrcColor: return vk::BlendFactor::eOneMinusSrcColor;
        case DstColor: return vk::BlendFactor::eDstColor;
        case OneMinusDstColor: return vk::BlendFactor::eOneMinusDstColor;
        case SrcAlpha: return vk::BlendFactor::eSrcAlpha;
        case OneMinusSrcAlpha: return vk::BlendFactor::eOneMinusSrcAlpha;
        case DstAlpha: return vk::BlendFactor::eDstAlpha;
        case OneMinusDstAlpha:
        default:
            return vk::BlendFactor::eOneMinusDstAlpha;
        }
    }

    inline vk::BlendOp GetVulkanBlendOpFromDesc(RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendOp op) {
        using enum RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendOp;
        switch (op) {
        case ADD: return vk::BlendOp::eAdd;
        case SUBTRACT: return vk::BlendOp::eSubtract;
        case ReverseSubtract:
        default:
            return vk::BlendOp::eReverseSubtract;
        }
    }

    class VulkanDevice;
    class VulkanPipelineState : public RHIPipelineState {
        const VulkanDevice& mDevice;

        vk::raii::Pipeline mPipeline{ nullptr };
        vk::raii::PipelineLayout mPipelineLayout{ nullptr };
        void InitializePipelineLayout();
        void InitializeGraphics();
        void InitializeCompute();
    public:
        VulkanPipelineState(const VulkanDevice& device, PipelineStateDesc const& desc);

        [[nodiscard]] inline auto const& GetVkPipeline() const { return mPipeline; }
        [[nodiscard]] inline auto const& GetVkPipelineLayout() const { return mPipelineLayout; }

        void DebugSetObjectName(const char* name) override;
    };
}
