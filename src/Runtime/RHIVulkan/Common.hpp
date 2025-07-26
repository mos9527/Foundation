#pragma once
#define VULKAN_HPP_NO_CONSTRUCTORS 
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <RHICore/Common.hpp>
#include <Core/Platform/Logging.hpp>
#include <Core/Container/Common.hpp>
namespace Foundation::RHI {
    // !! TODO: Properly handle bitmasks

    inline vk::Format vkFormatFromRHIFormat(RHIResourceFormat format) {
        using enum RHIResourceFormat;
        switch (format) {
        case R8G8B8A8_UNORM: return vk::Format::eR8G8B8A8Unorm;
        case R32_SIGNED_FLOAT: return vk::Format::eR32Sfloat;
        case R32G32_SIGNED_FLOAT: return vk::Format::eR32G32Sfloat;
        case R32G32B32_SIGNED_FLOAT: return vk::Format::eR32G32B32Sfloat;
        case R32G32B32A32_SIGNED_FLOAT: return vk::Format::eR32G32B32A32Sfloat;
        case R16_SIGNED_FLOAT: return vk::Format::eR16Sfloat;
        case R16G16_SIGNED_FLOAT: return vk::Format::eR16G16Sfloat;
        case R16G16B16_SIGNED_FLOAT: return vk::Format::eR16G16B16Sfloat;
        case R16G16B16A16_SIGNED_FLOAT: return vk::Format::eR16G16B16A16Sfloat;
        case R32_UINT: return vk::Format::eR32Uint;
        case R16_UINT: return vk::Format::eR16Uint;
        case D32_SIGNED_FLOAT: return vk::Format::eD32Sfloat;
        case Undefined:
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
        if (state & RenderTargetRead) flags |= vk::AccessFlagBits2::eColorAttachmentRead;
        if (state & DepthStencilWrite) flags |= vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        if (state & DepthStencilRead) flags |= vk::AccessFlagBits2::eDepthStencilAttachmentRead;
        if (state & TransferWrite) flags |= vk::AccessFlagBits2::eTransferWrite;
        if (state & ShaderRead) flags |= vk::AccessFlagBits2::eShaderRead;
        return flags;
    }

    inline vk::ImageLayout vkImageLayoutFromRHITextureLayout(RHITextureLayout layout) {
        switch (layout) {
        case RHITextureLayout::General: return vk::ImageLayout::eGeneral;
        case RHITextureLayout::RenderTarget: return vk::ImageLayout::eColorAttachmentOptimal;
        case RHITextureLayout::DepthStencil: return vk::ImageLayout::eDepthStencilAttachmentOptimal;
        case RHITextureLayout::Present: return vk::ImageLayout::ePresentSrcKHR;
        case RHITextureLayout::TransferDst: return vk::ImageLayout::eTransferDstOptimal;
        case RHITextureLayout::TransferSrc: return vk::ImageLayout::eTransferSrcOptimal;
        case RHITextureLayout::ShaderReadOnly: return vk::ImageLayout::eShaderReadOnlyOptimal;
        case RHITextureLayout::Undefined:
        default:
            return vk::ImageLayout::eUndefined;
        }
    }

    inline vk::PipelineStageFlags vkPipelineStageFlagsFromRHIPipelineStage(RHIPipelineStage stage) {
        using enum RHIPipelineStage;
        vk::PipelineStageFlags flags{};
        if (stage & TopOfPipe) flags |= vk::PipelineStageFlagBits::eTopOfPipe;
        if (stage & RenderTargetOutput) flags |= vk::PipelineStageFlagBits::eColorAttachmentOutput;
        if (stage & Transfer) flags |= vk::PipelineStageFlagBits::eTransfer;
        if (stage & FragmentShader) flags |= vk::PipelineStageFlagBits::eFragmentShader;
        if (stage & BottomOfPipe) flags |= vk::PipelineStageFlagBits::eBottomOfPipe;
        if (stage & DepthStencilRead) flags |= vk::PipelineStageFlagBits::eEarlyFragmentTests;
        if (stage & DepthStencilWrite) flags |= vk::PipelineStageFlagBits::eLateFragmentTests;
        return flags;
    }

    inline vk::PipelineStageFlags2 vkPipelineStageFlags2FromRHIPipelineStage(RHIPipelineStage stage) {
        using enum RHIPipelineStage;
        vk::PipelineStageFlags2 flags{};
        if (stage & TopOfPipe) flags |= vk::PipelineStageFlagBits2::eTopOfPipe;
        if (stage & RenderTargetOutput) flags |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        if (stage & Transfer) flags |= vk::PipelineStageFlagBits2::eTransfer;
        if (stage & FragmentShader) flags |= vk::PipelineStageFlagBits2::eFragmentShader;
        if (stage & BottomOfPipe) flags |= vk::PipelineStageFlagBits2::eBottomOfPipe;
        if (stage & DepthStencilRead) flags |= vk::PipelineStageFlagBits2::eEarlyFragmentTests;
        if (stage & DepthStencilWrite) flags |= vk::PipelineStageFlagBits2::eLateFragmentTests;
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
        case SampledImage:
            return vk::DescriptorType::eSampledImage;
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

    inline vk::ImageUsageFlags vkImageUsageFlagsFromRHITextureUsage(RHITextureUsage usage) {
        using enum RHITextureUsage;
        vk::ImageUsageFlags flags{};
        if (usage & RenderTarget) flags |= vk::ImageUsageFlagBits::eColorAttachment;
        if (usage & DepthStencil) flags |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        if (usage & SampledImage) flags |= vk::ImageUsageFlagBits::eSampled;
        if (usage & StorageImage) flags |= vk::ImageUsageFlagBits::eStorage;
        if (usage & TransferSource) flags |= vk::ImageUsageFlagBits::eTransferSrc;
        if (usage & TransferDestination) flags |= vk::ImageUsageFlagBits::eTransferDst;
        return flags;
    }

    inline vk::SampleCountFlagBits vkSampleCountFlagFromRHIMultisampleCount(RHIMultisampleCount count) {
        using enum RHIMultisampleCount;
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

    inline vk::ImageAspectFlags vkImageAspectFlagFromRHITextureAspect(RHITextureAccessFlag aspect) {
        using enum RHITextureAccessFlag;
        vk::ImageAspectFlags flags{};
        if (aspect & Color) flags |= vk::ImageAspectFlagBits::eColor;
        if (aspect & Depth) flags |= vk::ImageAspectFlagBits::eDepth;
        if (aspect & Stencil) flags |= vk::ImageAspectFlagBits::eStencil;
        return flags;
    }

}
