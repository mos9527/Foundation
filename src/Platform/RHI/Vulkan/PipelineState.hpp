#pragma once
#include <Platform/RHI/PipelineState.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            inline vk::PrimitiveTopology GetVulkanPrimitiveTopologyFromDesc(RHIPipelineState::PipelineStateDesc::Topology topology) {
                using enum RHIPipelineState::PipelineStateDesc::Topology;
                switch (topology) {
                    case POINT_LIST: return vk::PrimitiveTopology::ePointList;
                    case LINE_LIST: return vk::PrimitiveTopology::eLineList;
                    case TRIANGLE_STRIP: return vk::PrimitiveTopology::eTriangleStrip;
                    case TRIANGLE_LIST:
                    default:
                        return vk::PrimitiveTopology::eTriangleList;                       
                }
            }
            inline vk::PolygonMode GetVulkanPolygonModeFromDesc(RHIPipelineState::PipelineStateDesc::Rasterizer::FillMode mode) {
                switch (mode) {
                    case RHIPipelineState::PipelineStateDesc::Rasterizer::FILL_WIREFRAME: return vk::PolygonMode::eLine;
                    case RHIPipelineState::PipelineStateDesc::Rasterizer::FILL_SOLID: return vk::PolygonMode::eFill;
                    default:
                        return vk::PolygonMode::ePoint;
                }
            }

            inline vk::CullModeFlags GetVulkanCullModeFromDesc(RHIPipelineState::PipelineStateDesc::Rasterizer::CullMode mode) {
                switch (mode) {
                    case RHIPipelineState::PipelineStateDesc::Rasterizer::CULL_NONE: return vk::CullModeFlagBits::eNone;
                    case RHIPipelineState::PipelineStateDesc::Rasterizer::CULL_FRONT: return vk::CullModeFlagBits::eFront;
                    case RHIPipelineState::PipelineStateDesc::Rasterizer::CULL_BACK:
                    default:
                        return vk::CullModeFlagBits::eBack;
                }
            }

            inline vk::FrontFace GetVulkanFrontFaceFromDesc(RHIPipelineState::PipelineStateDesc::Rasterizer::FrontFace face) {
                switch (face) {
                    case RHIPipelineState::PipelineStateDesc::Rasterizer::FF_COUNTER_CLOCKWISE: return vk::FrontFace::eCounterClockwise;
                    case RHIPipelineState::PipelineStateDesc::Rasterizer::FF_CLOCKWISE:
                    default:
                        return vk::FrontFace::eClockwise;
                }
            }

            inline vk::SampleCountFlagBits GetVulkanSampleCountFromDesc(RHIPipelineState::PipelineStateDesc::Multisample::Count count) {
                using enum RHIPipelineState::PipelineStateDesc::Multisample::Count;
                switch (count) {
                    case e2: return vk::SampleCountFlagBits::e2;
                    case e4: return vk::SampleCountFlagBits::e4;
                    case e8: return vk::SampleCountFlagBits::e8;
                    case e16: return vk::SampleCountFlagBits::e16;
                    case e1: 
                    default:
                        return vk::SampleCountFlagBits::e1;
                }
            }

            inline vk::BlendFactor GetVulkanBlendFactorFromDesc(RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendFactor factor) {
                using enum RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendFactor;
                switch (factor) {
                    case ZERO: return vk::BlendFactor::eZero;
                    case ONE: return vk::BlendFactor::eOne;
                    case SRC_COLOR: return vk::BlendFactor::eSrcColor;
                    case ONE_MINUS_SRC_COLOR: return vk::BlendFactor::eOneMinusSrcColor;
                    case DST_COLOR: return vk::BlendFactor::eDstColor;
                    case ONE_MINUS_DST_COLOR: return vk::BlendFactor::eOneMinusDstColor;
                    case SRC_ALPHA: return vk::BlendFactor::eSrcAlpha;
                    case ONE_MINUS_SRC_ALPHA: return vk::BlendFactor::eOneMinusSrcAlpha;
                    case DST_ALPHA: return vk::BlendFactor::eDstAlpha;
                    case ONE_MINUS_DST_ALPHA:
                    default:
                        return vk::BlendFactor::eOneMinusDstAlpha;
                }
            }

            inline vk::BlendOp GetVulkanBlendOpFromDesc(RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendOp op) {
                using enum RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendOp;
                switch (op) {
                    case ADD: return vk::BlendOp::eAdd;
                    case SUBTRACT: return vk::BlendOp::eSubtract;
                    case REVERSE_SUBTRACT:
                    default:
                        return vk::BlendOp::eReverseSubtract;
                }
            }

            class VulkanDevice;
            class VulkanPipelineState : public RHIPipelineState {
                const VulkanDevice& m_device;

                vk::raii::Pipeline m_pipeline{ nullptr };
                vk::raii::PipelineLayout m_pipeline_layout{ nullptr };
            public:
                VulkanPipelineState(const VulkanDevice& device, PipelineStateDesc const& desc);

                inline bool IsValid() const { return true;  } // TODO
                inline const char* GetName() const { return m_desc.name.c_str(); }

                inline auto const& GetVkPipeline() const { return m_pipeline; }
                inline auto const& GetVkPipelineLayout() const { return m_pipeline_layout; }
            };
        }
    }
}
