#pragma once
#include <Math/Math.hpp>
#include <Bits/Enums.hpp>
#include "Details/Details.hpp"

namespace Foundation::RHI {
    constexpr static size_t kFullSize = -1;
    using RHIExtent1D = glm::vec<1, uint32_t>;
    using RHIExtent2D = glm::vec<2, uint32_t>;
    using RHIExtent3D = glm::vec<3, uint32_t>;
    using RHIOffset1D = glm::vec<1, int32_t>;
    using RHIOffset2D = glm::vec<2, int32_t>;
    using RHIOffset3D = glm::vec<3, int32_t>;
    using RHIClearColor = glm::vec<4, float>;
    using RHIClearDepthStencil = std::pair<float, uint32_t>;

    enum class RHIResourceFormat {
        Undefined = 0,
        R8G8B8A8_UNORM,
        R32_SIGNED_FLOAT,
        R32G32_SIGNED_FLOAT,
        R32G32B32_SIGNED_FLOAT,
        R32G32B32A32_SIGNED_FLOAT,
        R16_SIGNED_FLOAT,
        R16G16_SIGNED_FLOAT,
        R16G16B16_SIGNED_FLOAT,
        R16G16B16A16_SIGNED_FLOAT,
        R32_UINT,
        R16_UINT,
        D32_SIGNED_FLOAT
    };

    struct RHIVertexAttribute {
        uint32_t location; // Index into shader input
        uint32_t offset; // In bytes
        RHIResourceFormat format{ RHIResourceFormat::Undefined }; // Format on the GPU    
        uint32_t binding = 0; // 0-indexed index into bindings
    };

    enum class RHICommandPoolType {
        // The command pool is persistent, meaning command buffers can be reused
        Persistent,
        // The command pool is meant to be used once
        Transient,
    };
    enum class RHIDeviceQueueType {
        Graphics,
        Compute,
        Transfer,
        Present
    };
    enum class RHIDevicePipelineType {
        Graphics,
        Compute,
    };
    enum class RHIDeviceHeapType {
        Local,
        Upload,
        Readback
    };

    enum class RHITextureLayout {
        Undefined,
        General,
        RenderTarget,
        DepthStencil,
        Present,
        TransferDst,
        TransferSrc,
        ShaderReadOnly,
    };

    enum class RHIResourceHostAccess {
        Invisible,
        ReadWrite, // r/w are possible
        WriteOnly // write only, reads are undefined
    };

    enum class RHIDescriptorType {
        Sampler,
        SampledImage,
        StorageImage,
        UniformBuffer,
        StorageBuffer
    };

    ENUM_NAME_CONV_BEGIN(RHIDescriptorType)
        case Sampler: return "Sampler";
        case SampledImage: return "SampledImage";
        case StorageImage: return "StorageImage";
        case UniformBuffer: return "UniformBuffer";
        case StorageBuffer: return "StorageBuffe";
    ENUM_NAME_CONV_END()

    enum class RHIMultisampleCount {
        e1, e2, e4, e8, e16
    };
    enum class RHITextureDimension {
        e1D, e2D, e3D
    };

    BITMASK_ENUM_BEGIN(RHIShaderStage, uint32_t)
        Vertex = 1 << 0,
        Fragment = 1 << 1,
        Compute = 1 << 2,
        All = ~0u
    BITMASK_ENUM_END();

    ENUM_NAME_CONV_BEGIN(RHIShaderStageBits)
        case Vertex: return "Vertex Stage";
        case Fragment: return "Fragment Stage";
        case Compute: return "Compute Stage";
    ENUM_NAME_CONV_END()
        
    BITMASK_ENUM_BEGIN(RHIResourceAccess, uint32_t)
        RenderTargetWrite = 1 << 0,
        RenderTargetRead = 1 << 1,
        DepthStencilWrite = 1 << 2,
        DepthStencilRead = 1 << 3,
        TransferWrite = 1 << 4,
        TransferRead = 1 << 5,
        ShaderWrite = 1 << 6,
        ShaderRead = 1 << 7,
        UniformRead = 1 << 8,
    BITMASK_ENUM_END();

    // https://gpuopen.com/learn/vulkan-barriers-explained/
    // https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-pipeline-barriers   
    BITMASK_ENUM_BEGIN(RHIPipelineStage, uint32_t)            
        DrawIndirect            = 1 << 1,
        VertexShader            = 1 << 2,
        FragmentShader          = 1 << 3,
        ComputeShader           = 1 << 4,
        RayTracingShader        = 1 << 5,
        MeshShader              = 1 << 6,
        ColorAttachmentOutput   = 1 << 7,
        Transfer                = 1 << 8,
        EarlyFragmentTests      = 1 << 9,
        LateFragmentTests       = 1 << 10,
        // ---
        TopOfPipe               = 1 << 29,
        BottomOfPipe            = 1 << 30,
    BITMASK_ENUM_END();

    BITMASK_ENUM_BEGIN(RHIBufferUsage, uint32_t)
        VertexBuffer = 1 << 0,
        IndexBuffer = 1 << 1,
        // i.e. Uniform Buffer
        UniformBuffer = 1 << 2,
        // i.e. Structured Buffer
        StorageBuffer = 1 << 3,
        IndirectBuffer = 1 << 4,
        TransferSource = 1 << 5,
        TransferDestination = 1 << 6
    BITMASK_ENUM_END();

    BITMASK_ENUM_BEGIN(RHITextureUsage, uint32_t)
        RenderTarget = 1 << 0,
        DepthStencil = 1 << 1,
        SampledImage = 1 << 2,
        StorageImage = 1 << 3,
        TransferSource = 1 << 4,
        TransferDestination = 1 << 5
    BITMASK_ENUM_END();

    BITMASK_ENUM_BEGIN(RHITextureAccessFlag, uint32_t)
        Color = 1 << 0,
        Depth = 1 << 1,
        Stencil = 1 << 2
    BITMASK_ENUM_END();
}
