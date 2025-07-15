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
                    default: return vk::Format::eUndefined;
                }
            }
            inline vk::BufferUsageFlags vkBufferUsageFromRHIBufferUsage(RHIBufferUsage usage) {
                using enum RHIBufferUsage;
                vk::BufferUsageFlags flags{};
                if (usage & VertexBuffer) flags |= vk::BufferUsageFlagBits::eVertexBuffer;
                if (usage & IndexBuffer) flags |= vk::BufferUsageFlagBits::eIndexBuffer;
                if (usage & ConstantBuffer) flags |= vk::BufferUsageFlagBits::eUniformBuffer;
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
                    case RHIImageLayout::Undefined: return vk::ImageLayout::eUndefined;
                    case RHIImageLayout::General: return vk::ImageLayout::eGeneral;
                    case RHIImageLayout::RenderTarget: return vk::ImageLayout::eColorAttachmentOptimal;
                    case RHIImageLayout::DepthStencil: return vk::ImageLayout::eDepthStencilAttachmentOptimal;
                    case RHIImageLayout::Present: return vk::ImageLayout::ePresentSrcKHR;
                    default: return vk::ImageLayout::eUndefined;
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
        }
    }
}
