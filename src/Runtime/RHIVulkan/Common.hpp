#pragma once
#define VULKAN_HPP_NO_CONSTRUCTORS 
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <RHICore/Common.hpp>
namespace Foundation::RHI {
    template<typename Bits> inline Bits vkFlagsToBits(vk::Flags<Bits> flags) {
        return static_cast<Bits>(static_cast<std::underlying_type_t<Bits>>(flags));
    }
    inline vk::Format vkFormatFromRHIFormat(RHIResourceFormat format) {
        using enum RHIResourceFormat;
        switch (format) {
        case R8G8B8A8_UNORM: return vk::Format::eR8G8B8A8Unorm;
        case R8G8B8A8_SRGB: return vk::Format::eR8G8B8A8Srgb;
        case B8G8R8A8_UNROM: return vk::Format::eB8G8R8A8Unorm;
        case B8G8R8A8_SRGB: return vk::Format::eB8G8R8A8Srgb;
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
        using enum RHIBufferUsageBits;
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
        using enum RHIResourceAccessBits;
        vk::AccessFlags2 flags{};
        if (state & RenderTargetWrite) flags |= vk::AccessFlagBits2::eColorAttachmentWrite;
        if (state & RenderTargetRead) flags |= vk::AccessFlagBits2::eColorAttachmentRead;
        if (state & DepthStencilWrite) flags |= vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
        if (state & DepthStencilRead) flags |= vk::AccessFlagBits2::eDepthStencilAttachmentRead;
        if (state & TransferWrite) flags |= vk::AccessFlagBits2::eTransferWrite;
        if (state & TransferRead) flags |= vk::AccessFlagBits2::eTransferRead;
        if (state & ShaderWrite) flags |= vk::AccessFlagBits2::eShaderWrite;
        if (state & ShaderRead) flags |= vk::AccessFlagBits2::eShaderRead;
        if (state & UniformRead) flags |= vk::AccessFlagBits2::eUniformRead;
        return flags;
    }

    inline vk::ImageLayout vkImageLayoutFromRHITextureLayout(RHITextureLayout layout) {
        switch (layout) {
        case RHITextureLayout::Undefined: return vk::ImageLayout::eUndefined;
        case RHITextureLayout::General: return vk::ImageLayout::eGeneral;
        case RHITextureLayout::RenderTarget: return vk::ImageLayout::eColorAttachmentOptimal;
        case RHITextureLayout::DepthStencil: return vk::ImageLayout::eDepthStencilAttachmentOptimal;
        case RHITextureLayout::Present: return vk::ImageLayout::ePresentSrcKHR;
        case RHITextureLayout::TransferDst: return vk::ImageLayout::eTransferDstOptimal;
        case RHITextureLayout::TransferSrc: return vk::ImageLayout::eTransferSrcOptimal;
        case RHITextureLayout::ShaderReadOnly: return vk::ImageLayout::eShaderReadOnlyOptimal;
        default:
            return vk::ImageLayout::eUndefined;
        }
    }

    inline vk::PipelineStageFlags vkPipelineStageFlagsFromRHIPipelineStage(RHIPipelineStage stage) {
        using enum RHIPipelineStageBits;
        vk::PipelineStageFlags flags{};
        if (stage & DrawIndirect) flags |= vk::PipelineStageFlagBits::eDrawIndirect;
        if (stage & FragmentShader) flags |= vk::PipelineStageFlagBits::eFragmentShader;
        if (stage & VertexShader) flags |= vk::PipelineStageFlagBits::eVertexShader;
        if (stage & ComputeShader) flags |= vk::PipelineStageFlagBits::eComputeShader;
        if (stage & RayTracingShader) flags |= vk::PipelineStageFlagBits::eRayTracingShaderKHR;
        if (stage & MeshShader) flags |= vk::PipelineStageFlagBits::eMeshShaderEXT;
        if (stage & ColorAttachmentOutput) flags |= vk::PipelineStageFlagBits::eColorAttachmentOutput;
        if (stage & Transfer) flags |= vk::PipelineStageFlagBits::eTransfer;
        if (stage & EarlyFragmentTests) flags |= vk::PipelineStageFlagBits::eEarlyFragmentTests;
        if (stage & LateFragmentTests) flags |= vk::PipelineStageFlagBits::eLateFragmentTests;
        if (stage & TopOfPipe) flags |= vk::PipelineStageFlagBits::eTopOfPipe;
        if (stage & BottomOfPipe) flags |= vk::PipelineStageFlagBits::eBottomOfPipe;
        if (stage & AllGraphics) flags |= vk::PipelineStageFlagBits::eAllGraphics;
        return flags;
    }

    inline vk::PipelineStageFlags2 vkPipelineStageFlags2FromRHIPipelineStage(RHIPipelineStage stage) {
        using enum RHIPipelineStageBits;
        vk::PipelineStageFlags2 flags{};
        if (stage & DrawIndirect) flags |= vk::PipelineStageFlagBits2::eDrawIndirect;
        if (stage & FragmentShader) flags |= vk::PipelineStageFlagBits2::eFragmentShader;
        if (stage & VertexShader) flags |= vk::PipelineStageFlagBits2::eVertexShader;
        if (stage & ComputeShader) flags |= vk::PipelineStageFlagBits2::eComputeShader;
        if (stage & RayTracingShader) flags |= vk::PipelineStageFlagBits2::eRayTracingShaderKHR;
        if (stage & MeshShader) flags |= vk::PipelineStageFlagBits2::eMeshShaderEXT;
        if (stage & ColorAttachmentOutput) flags |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        if (stage & Transfer) flags |= vk::PipelineStageFlagBits2::eTransfer;
        if (stage & EarlyFragmentTests) flags |= vk::PipelineStageFlagBits2::eEarlyFragmentTests;
        if (stage & LateFragmentTests) flags |= vk::PipelineStageFlagBits2::eLateFragmentTests;
        if (stage & TopOfPipe) flags |= vk::PipelineStageFlagBits2::eTopOfPipe;
        if (stage & BottomOfPipe) flags |= vk::PipelineStageFlagBits2::eBottomOfPipe;
        if (stage & AllGraphics) flags |= vk::PipelineStageFlagBits2::eAllGraphics;
        return flags;
    }

    inline vk::ShaderStageFlags vkShaderStageFlagsFromRHIShaderStage(RHIShaderStage stage) {
        using enum RHIShaderStageBits;
        vk::ShaderStageFlags flags{};
        if (stage == All) return vk::ShaderStageFlagBits::eAll;
        if (stage & Vertex) flags |= vk::ShaderStageFlagBits::eVertex;
        if (stage & Fragment) flags |= vk::ShaderStageFlagBits::eFragment;
        if (stage & Compute) flags |= vk::ShaderStageFlagBits::eCompute;
        return flags;
    }

    inline vk::DescriptorType vkDescriptorTypeFromRHIDescriptorType(RHIDescriptorType type) {
        using enum RHIDescriptorType;
        switch (type)
        {
        case Sampler:
            return vk::DescriptorType::eSampler;
        case SampledImage:
            return vk::DescriptorType::eSampledImage;
        case StorageImage:
            return vk::DescriptorType::eStorageImage;
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
        using enum RHITextureUsageBits;
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

    inline vk::ImageAspectFlags vkImageAspectFlagFromRHITextureAspect(RHITextureAspectFlag aspect) {
        using enum RHITextureAccessFlagBits;
        vk::ImageAspectFlags flags{};
        if (aspect & Color) flags |= vk::ImageAspectFlagBits::eColor;
        if (aspect & Depth) flags |= vk::ImageAspectFlagBits::eDepth;
        if (aspect & Stencil) flags |= vk::ImageAspectFlagBits::eStencil;
        return flags;
    }

}
