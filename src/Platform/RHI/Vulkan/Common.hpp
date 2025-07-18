#pragma once
#define VULKAN_HPP_NO_CONSTRUCTORS 
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <Platform/RHI/Common.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            inline vk::Format vkFormatFromRHIFormat(RHIResourceFormat format) {
                switch (format) {
                    case RHIResourceFormat::R8G8B8A8_UNORM: return vk::Format::eR8G8B8A8Unorm;
                    case RHIResourceFormat::R32_SIGNED_FLOAT: return vk::Format::eR32Sfloat;
                    case RHIResourceFormat::R32G32_SIGNED_FLOAT: return vk::Format::eR32G32Sfloat;
                    case RHIResourceFormat::R32G32B32_SIGNED_FLOAT: return vk::Format::eR32G32B32Sfloat;
                    case RHIResourceFormat::R32G32B32A32_SIGNED_FLOAT: return vk::Format::eR32G32B32A32Sfloat;
                    case RHIResourceFormat::Undefined:
                    default:
                        return vk::Format::eUndefined;
                }
            }
            inline vk::BufferUsageFlags vkBufferUsageFromRHIBufferUsage(RHIBufferUsage usage) {
                using enum RHIBufferUsage;
                vk::BufferUsageFlags flags{};
                if (usage & VertexBuffer) flags |= vk::BufferUsageFlagBits::eVertexBuffer;
                if (usage & IndexBuffer) flags |= vk::BufferUsageFlagBits::eIndexBuffer;
                if (usage & UniformBuffer) flags |= vk::BufferUsageFlagBits::eUniformBuffer;
                if (usage & StorageBuffer) flags |= vk::BufferUsageFlagBits::eStorageBuffer;
                if (usage & IndirectBuffer) flags |= vk::BufferUsageFlagBits::eIndirectBuffer;
                if (usage & TransferSource) flags |= vk::BufferUsageFlagBits::eTransferSrc;
                if (usage & TransferDestination) flags |= vk::BufferUsageFlagBits::eTransferDst;
                return flags;
            }

            inline vk::AccessFlags2 vkAccessFlagsFromRHIResourceAccess(RHIResourceAccess state) {
                using enum RHIResourceAccess;
                vk::AccessFlags2 flags{};
                if (state & RenderTargetWrite) flags |= vk::AccessFlagBits2::eColorAttachmentWrite;
                return flags;
            }

            inline vk::ImageLayout vkImageLayoutFromRHIImageLayout(RHIImageLayout layout) {
                switch (layout) {
                    case RHIImageLayout::General: return vk::ImageLayout::eGeneral;
                    case RHIImageLayout::RenderTarget: return vk::ImageLayout::eColorAttachmentOptimal;
                    case RHIImageLayout::DepthStencil: return vk::ImageLayout::eDepthStencilAttachmentOptimal;
                    case RHIImageLayout::Present: return vk::ImageLayout::ePresentSrcKHR;
                    case RHIImageLayout::Undefined:
                    default:
                        return vk::ImageLayout::eUndefined;
                }
            }

            inline vk::PipelineStageFlags vkPipelineStageFlagsFromRHIPipelineStage(RHIPipelineStage stage) {
                using enum RHIPipelineStage;
                vk::PipelineStageFlags flags{};
                if (stage & TopOfPipe) flags |= vk::PipelineStageFlagBits::eTopOfPipe;
                if (stage & RenderTargetOutput) flags |= vk::PipelineStageFlagBits::eColorAttachmentOutput;
                if (stage & BottomOfPipe) flags |= vk::PipelineStageFlagBits::eBottomOfPipe;
                return flags;
            }

            inline vk::PipelineStageFlags2 vkPipelineStageFlags2FromRHIPipelineStage(RHIPipelineStage stage) {
                using enum RHIPipelineStage;
                vk::PipelineStageFlags2 flags{};
                if (stage & TopOfPipe) flags |= vk::PipelineStageFlagBits2::eTopOfPipe;
                if (stage & RenderTargetOutput) flags |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
                if (stage & BottomOfPipe) flags |= vk::PipelineStageFlagBits2::eBottomOfPipe;
                return flags;
            }

            inline vk::ShaderStageFlags vkShaderStageFlagsFromRHIShaderStage(RHIShaderStage stage) {
                using enum RHIShaderStage;
                vk::ShaderStageFlags flags{};
                if (stage == All) return vk::ShaderStageFlagBits::eAll;
                if (stage & Vertex) flags |= vk::ShaderStageFlagBits::eVertex;
                if (stage & Fragment) flags |= vk::ShaderStageFlagBits::eFragment;
                if (stage & Compute) flags |= vk::ShaderStageFlagBits::eCompute;
                return flags;
            }

            // XXX: Returns only the first matching flag bit. We don't have
            // a 'bit' type per-se.
            // Documentation has been explicitly stating that multiple bitmasks
            // would result in UB - but programmers are stupid (I'm one of them)
            // and shit like this would be impossible to debug.
            // TODO: Bit types for singular flags.
            inline vk::ShaderStageFlagBits vkShaderStageFlagBitFromRHIShaderStage(RHIShaderStage stage) {
                using enum RHIShaderStage;
                if (stage & Vertex) return vk::ShaderStageFlagBits::eVertex;
                if (stage & Fragment) return vk::ShaderStageFlagBits::eFragment;
                if (stage & Compute) return vk::ShaderStageFlagBits::eCompute;
                return {};
            }

            inline vk::DescriptorType vkDescriptorTypeFromRHIDescriptorType(RHIDescriptorType type) {
                using enum RHIDescriptorType;
                switch (type)
                {
                case Sampler:                                    
                    return vk::DescriptorType::eSampler;
                case StorageBuffer:
                    return vk::DescriptorType::eStorageBuffer;
                default:
                case UniformBuffer:
                    return vk::DescriptorType::eUniformBuffer;
                }
            }

            inline vk::PipelineBindPoint vkPipelineBindPointFromRHIDevicePipelineType(RHIDevicePipelineType type) {
                switch (type) {
                    case RHIDevicePipelineType::Compute:  return vk::PipelineBindPoint::eCompute;
                    default:
                    case RHIDevicePipelineType::Graphics: return vk::PipelineBindPoint::eGraphics;
                }
            }
        }
    }
}
