#pragma once
#define VULKAN_HPP_NO_CONSTRUCTORS 
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <RHICore/Common.hpp>
namespace Foundation::RHI {
    struct VulkanAllocationCallbacks
    {
        Allocator* allocator{nullptr};
        VkAllocationCallbacks callbacks{};

        explicit VulkanAllocationCallbacks(Allocator* allocator = nullptr)
        {
            Reset(allocator);
        }

        void Reset(Allocator* newAllocator)
        {
            allocator = newAllocator;
            callbacks = VkAllocationCallbacks{
                .pUserData = allocator,
                .pfnAllocation = &VulkanAllocationCallbacks::Allocate,
                .pfnReallocation = &VulkanAllocationCallbacks::Reallocate,
                .pfnFree = &VulkanAllocationCallbacks::Free,
            };
        }

        [[nodiscard]] vk::AllocationCallbacks const* Get() const
        {
            return allocator ? reinterpret_cast<vk::AllocationCallbacks const*>(&callbacks) : nullptr;
        }

        [[nodiscard]] VkAllocationCallbacks const* GetNative() const
        {
            return allocator ? &callbacks : nullptr;
        }

    private:
        static void* VKAPI_PTR Allocate(void* userData, size_t size, size_t alignment,
                                        VkSystemAllocationScope)
        {
            if (!userData || size == 0)
                return nullptr;
            try
            {
                return static_cast<Allocator*>(userData)->Allocate(size, alignment ? alignment : alignof(std::max_align_t));
            }
            catch (...)
            {
                return nullptr;
            }
        }

        static void* VKAPI_PTR Reallocate(void* userData, void* original, size_t size, size_t alignment,
                                          VkSystemAllocationScope)
        {
            if (!userData)
                return nullptr;
            auto* allocator = static_cast<Allocator*>(userData);
            try
            {
                if (size == 0)
                {
                    allocator->Deallocate(original);
                    return nullptr;
                }
                if (!original)
                    return allocator->Allocate(size, alignment ? alignment : alignof(std::max_align_t));
                return allocator->Reallocate(original, size, alignment ? alignment : alignof(std::max_align_t));
            }
            catch (...)
            {
                return nullptr;
            }
        }

        static void VKAPI_PTR Free(void* userData, void* memory)
        {
            if (userData && memory)
                static_cast<Allocator*>(userData)->Deallocate(memory);
        }
    };

    template<typename Bits> Bits vkFlagsToBits(vk::Flags<Bits> flags) {
        return static_cast<Bits>(static_cast<std::underlying_type_t<Bits>>(flags));
    }
    inline vk::Format vkFormatFromRHIFormat(RHIResourceFormat format) {
        using enum RHIResourceFormat;
        switch (format) {
        case R8G8B8A8Unorm: return vk::Format::eR8G8B8A8Unorm;
        case R8G8B8A8Srgb: return vk::Format::eR8G8B8A8Srgb;
        case B8G8R8A8Unrom: return vk::Format::eB8G8R8A8Unorm;
        case B8G8R8A8Srgb: return vk::Format::eB8G8R8A8Srgb;
        case A2R10G10B10Unorm: return vk::Format::eA2R10G10B10UnormPack32;
        case A2R10G10B10Snorm: return vk::Format::eA2R10G10B10SnormPack32;
        case A2B10G10R10Unorm: return vk::Format::eA2B10G10R10UnormPack32;
        case A2B10G10R10Snorm: return vk::Format::eA2B10G10R10SnormPack32;
        case B10G11R11Ufloat: return vk::Format::eB10G11R11UfloatPack32;
        case R32SignedFloat: return vk::Format::eR32Sfloat;
        case R32G32SignedFloat: return vk::Format::eR32G32Sfloat;
        case R32G32B32SignedFloat: return vk::Format::eR32G32B32Sfloat;
        case R32G32B32A32SignedFloat: return vk::Format::eR32G32B32A32Sfloat;
        case R16SignedFloat: return vk::Format::eR16Sfloat;
        case R16G16SignedFloat: return vk::Format::eR16G16Sfloat;
        case R16G16B16SignedFloat: return vk::Format::eR16G16B16Sfloat;
        case R16G16B16A16SignedFloat: return vk::Format::eR16G16B16A16Sfloat;
        case R32Uint: return vk::Format::eR32Uint;
        case R16Uint: return vk::Format::eR16Uint;
        case R16Unorm: return vk::Format::eR16Unorm;
        case D32SignedFloat: return vk::Format::eD32Sfloat;
        case D16Unorm: return vk::Format::eD16Unorm;
        case Bc1RgbUnorm: return vk::Format::eBc1RgbUnormBlock;
        case Bc1RgbSrgb: return vk::Format::eBc1RgbSrgbBlock;
        case Bc1RgbaUnorm: return vk::Format::eBc1RgbaUnormBlock;
        case Bc1RgbaSrgb: return vk::Format::eBc1RgbaSrgbBlock;
        case Bc2Unorm: return vk::Format::eBc2UnormBlock;
        case Bc2Srgb: return vk::Format::eBc2SrgbBlock;
        case Bc3Unorm: return vk::Format::eBc3UnormBlock;
        case Bc3Srgb: return vk::Format::eBc3SrgbBlock;
        case Bc4Unorm: return vk::Format::eBc4UnormBlock;
        case Bc4Snorm: return vk::Format::eBc4SnormBlock;
        case Bc5Unorm: return vk::Format::eBc5UnormBlock;
        case Bc5Snorm: return vk::Format::eBc5SnormBlock;
        case Bc6HUfloat: return vk::Format::eBc6HUfloatBlock;
        case Bc6HSfloat: return vk::Format::eBc6HSfloatBlock;
        case Bc7Unorm: return vk::Format::eBc7UnormBlock;
        case Bc7Srgb: return vk::Format::eBc7SrgbBlock;
        case Undefined:
        default:
            return vk::Format::eUndefined;
        }
    }

    inline vk::ColorSpaceKHR vkColorSpaceFromRHIColorSpace(RHIColorSpace colorSpace) {
        using enum RHIColorSpace;
        switch (colorSpace) {
        case SrgbNonLinear: return vk::ColorSpaceKHR::eSrgbNonlinear;
        case ExtendedSrgbLinear: return vk::ColorSpaceKHR::eExtendedSrgbLinearEXT;
        case Hdr10St2084: return vk::ColorSpaceKHR::eHdr10St2084EXT;
        default:
            return vk::ColorSpaceKHR::eSrgbNonlinear;
        }
    }

    inline RHIColorSpace rhiColorSpaceFromVkColorSpace(vk::ColorSpaceKHR colorSpace) {
        using enum RHIColorSpace;
        switch (colorSpace) {
        case vk::ColorSpaceKHR::eSrgbNonlinear: return SrgbNonLinear;
        case vk::ColorSpaceKHR::eExtendedSrgbLinearEXT: return ExtendedSrgbLinear;
        case vk::ColorSpaceKHR::eHdr10St2084EXT: return Hdr10St2084;
        default:
            return SrgbNonLinear;
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
        if (usage & DeviceAddress) flags |= vk::BufferUsageFlagBits::eShaderDeviceAddress;
        if (usage & AccelerationStructureStorage) flags |= vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR;
        if (usage & AccelerationStructureBuildReadOnly) flags |= vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR;
        if (usage & ShaderBindingTable) flags |= vk::BufferUsageFlagBits::eShaderBindingTableKHR;
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
        if (state & HostWrite) flags |= vk::AccessFlagBits2::eHostWrite;
        if (state & HostRead) flags |= vk::AccessFlagBits2::eHostRead;
        if (state & AccelerationStructureRead) flags |= vk::AccessFlagBits2::eAccelerationStructureReadKHR;
        if (state & AccelerationStructureWrite) flags |= vk::AccessFlagBits2::eAccelerationStructureWriteKHR;
        if (state & IndirectCommandRead) flags |= vk::AccessFlagBits2::eIndirectCommandRead;
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
        if (stage & RenderTargetOutput) flags |= vk::PipelineStageFlagBits::eColorAttachmentOutput;
        if (stage & Transfer) flags |= vk::PipelineStageFlagBits::eTransfer;
        if (stage & EarlyFragmentTests) flags |= vk::PipelineStageFlagBits::eEarlyFragmentTests;
        if (stage & LateFragmentTests) flags |= vk::PipelineStageFlagBits::eLateFragmentTests;
        if (stage & AccelerationBuild) flags |= vk::PipelineStageFlagBits::eAccelerationStructureBuildKHR;
        if (stage & TopOfPipe) flags |= vk::PipelineStageFlagBits::eTopOfPipe;
        if (stage & BottomOfPipe) flags |= vk::PipelineStageFlagBits::eBottomOfPipe;
        if (stage & AllGraphics) flags |= vk::PipelineStageFlagBits::eAllGraphics;
        if (stage & Host) flags |= vk::PipelineStageFlagBits::eHost;
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
        if (stage & TaskShader) flags |= vk::PipelineStageFlagBits2::eTaskShaderEXT;
        if (stage & RenderTargetOutput) flags |= vk::PipelineStageFlagBits2::eColorAttachmentOutput;
        if (stage & Transfer) flags |= vk::PipelineStageFlagBits2::eTransfer;
        if (stage & EarlyFragmentTests) flags |= vk::PipelineStageFlagBits2::eEarlyFragmentTests;
        if (stage & LateFragmentTests) flags |= vk::PipelineStageFlagBits2::eLateFragmentTests;
        if (stage & AccelerationBuild) flags |= vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR;
        if (stage & TopOfPipe) flags |= vk::PipelineStageFlagBits2::eTopOfPipe;
        if (stage & BottomOfPipe) flags |= vk::PipelineStageFlagBits2::eBottomOfPipe;
        if (stage & AllGraphics) flags |= vk::PipelineStageFlagBits2::eAllGraphics;
        if (stage & Host) flags |= vk::PipelineStageFlagBits2::eHost;
        return flags;
    }

    inline vk::ShaderStageFlags vkShaderStageFlagsFromRHIShaderStage(RHIShaderStage stage) {
        using enum RHIShaderStageBits;
        vk::ShaderStageFlags flags{};
        if (stage == All) return vk::ShaderStageFlagBits::eAll;
        if (stage & Vertex) flags |= vk::ShaderStageFlagBits::eVertex;
        if (stage & Fragment) flags |= vk::ShaderStageFlagBits::eFragment;
        if (stage & Compute) flags |= vk::ShaderStageFlagBits::eCompute;
        if (stage & RayGeneration) flags |= vk::ShaderStageFlagBits::eRaygenKHR;
        if (stage & RayAnyHit) flags |= vk::ShaderStageFlagBits::eAnyHitKHR;
        if (stage & RayClosestHit) flags |= vk::ShaderStageFlagBits::eClosestHitKHR;
        if (stage & RayMiss) flags |= vk::ShaderStageFlagBits::eMissKHR;
        if (stage & RayIntersection) flags |= vk::ShaderStageFlagBits::eIntersectionKHR;
        if (stage & Task) flags |= vk::ShaderStageFlagBits::eTaskEXT;
        if (stage & Mesh) flags |= vk::ShaderStageFlagBits::eMeshEXT;
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
        case UniformBuffer:
            return vk::DescriptorType::eUniformBuffer;
        case AccelerationStructure:
            return vk::DescriptorType::eAccelerationStructureKHR;
        default:
            return {};
        }
    }

    inline vk::PipelineBindPoint vkPipelineBindPointFromRHIDevicePipelineType(RHIDevicePipelineType type) {
        switch (type) {
        case RHIDevicePipelineType::Compute:  return vk::PipelineBindPoint::eCompute;
        case RHIDevicePipelineType::Graphics: return vk::PipelineBindPoint::eGraphics;
        case RHIDevicePipelineType::RayTracing: return vk::PipelineBindPoint::eRayTracingKHR;
        default:
            return {};
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
        case E2: return vk::SampleCountFlagBits::e2;
        case E4: return vk::SampleCountFlagBits::e4;
        case E8: return vk::SampleCountFlagBits::e8;
        case E16: return vk::SampleCountFlagBits::e16;
        case E1: return vk::SampleCountFlagBits::e1;
        default:
            return {};
        }
    }

    inline vk::ImageAspectFlags vkImageAspectFlagFromRHITextureAspect(RHITextureAspectFlag aspect) {
        using enum RHITextureAspectFlagBits;
        vk::ImageAspectFlags flags{};
        if (aspect & Color) flags |= vk::ImageAspectFlagBits::eColor;
        if (aspect & Depth) flags |= vk::ImageAspectFlagBits::eDepth;
        if (aspect & Stencil) flags |= vk::ImageAspectFlagBits::eStencil;
        return flags;
    }

    inline vk::AccelerationStructureTypeKHR vkAccelerationStructureTypeFromRHIAccelerationStructureType(RHIAccelerationStructureType type) {
        using enum RHIAccelerationStructureType;
        switch (type) {
        case TopLevel: return vk::AccelerationStructureTypeKHR::eTopLevel;
        case BottomLevel: return vk::AccelerationStructureTypeKHR::eBottomLevel;
        default:
            return {};
        }
    }

    inline vk::BuildAccelerationStructureModeKHR vkBuildAccelerationStructureModeFromRHIAccelerationStructureBuildOp(RHIAccelerationStructureBuildOp op)
    {
        using enum RHIAccelerationStructureBuildOp;
        switch (op) {
        case Build: return vk::BuildAccelerationStructureModeKHR::eBuild;
        case Update: return vk::BuildAccelerationStructureModeKHR::eUpdate;
        default:
            return {};
        }
    }

    inline vk::BuildAccelerationStructureFlagsKHR vkBuildAccelerationStructureFlagsFromRHIAccelerationStructureBuildFlags(RHIAccelerationStructureBuildFlags flags)
    {
        using enum RHIAccelerationStructureBuildFlagsBits;
        vk::BuildAccelerationStructureFlagsKHR vkFlags{};
        if (flags & AllowUpdate) vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowUpdate;
        if (flags & AllowCompaction) vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::eAllowCompaction;
        if (flags & PreferFastTrace) vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
        if (flags & PreferFastBuild) vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastBuild;
        if (flags & LowMemory) vkFlags |= vk::BuildAccelerationStructureFlagBitsKHR::eLowMemory;
        return vkFlags;
    }
}
