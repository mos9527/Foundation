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
        case RHIPipelineState::PipelineStateDesc::Rasterizer::FFCounterClockwise: return vk::FrontFace::eCounterClockwise;
        case RHIPipelineState::PipelineStateDesc::Rasterizer::FFClockwise:
        default:
            return vk::FrontFace::eClockwise;
        }
    }

    inline vk::BlendFactor GetVulkanBlendFactorFromDesc(RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendFactor factor) {
        using enum RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendFactor;
        switch (factor) {
        case Zero: return vk::BlendFactor::eZero;
        case One: return vk::BlendFactor::eOne;
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
        case Add: return vk::BlendOp::eAdd;
        case Subtract: return vk::BlendOp::eSubtract;
        case ReverseSubtract:
        default:
            return vk::BlendOp::eReverseSubtract;
        }
    }

    class VulkanDevice;
    class VulkanPipelineStateCache : public RHIPipelineStateCache
    {
        const VulkanDevice& mDevice;

        vk::raii::PipelineCache mCache{nullptr};
    public:
        VulkanPipelineStateCache(const VulkanDevice& device, PipelineStateCacheDesc const& desc);

        [[nodiscard]] auto const& GetVkPipelineCache() { return mCache; }

        [[nodiscard]] size_t GetCachedData(void* dstBuffer) const override;
        void DebugSetObjectName(const char* name) override;
    };
    struct VulkanPipelineRayTracingSBT
    {
        VkStridedDeviceAddressRegionKHR raygen{};
        VkStridedDeviceAddressRegionKHR miss{};
        VkStridedDeviceAddressRegionKHR hit{};
        VkStridedDeviceAddressRegionKHR callable{};
    };
    class VulkanPipelineState : public RHIPipelineState {
        VulkanDevice& mDevice;

        vk::raii::Pipeline mPipeline{ nullptr };
        vk::raii::PipelineLayout mPipelineLayout{ nullptr };
        void InitializePipelineLayout();
        void InitializeGraphics();
        void InitializeCompute();
        void InitializeRayTracing();

        RHIDeviceScopedHandle<RHIBuffer> mSBTBuffer;
        VulkanPipelineRayTracingSBT mSBT;
    public:
        VulkanPipelineState(VulkanDevice& device, PipelineStateDesc const& desc);

        [[nodiscard]] auto const& GetVkPipeline() const { return mPipeline; }
        [[nodiscard]] auto const& GetVkPipelineLayout() const { return mPipelineLayout; }
        [[nodiscard]] VulkanPipelineRayTracingSBT const& GetVkSBT() const { return mSBT; }

        void DebugSetObjectName(const char* name) override;
    };
}
