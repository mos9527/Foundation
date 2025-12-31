#pragma once
#include <Core/Container.hpp>
#include <Core/Enums.hpp>
#include <Math/Math.hpp>
#include "Details.hpp"
namespace Foundation::RHI {
    using namespace Core;
    constexpr static size_t kFullSize = -1;
    using RHIExtent1D = glm::vec<1, uint32_t>;
    using RHIExtent2D = glm::vec<2, uint32_t>;
    using RHIExtent3D = glm::vec<3, uint32_t>;
    using RHIOffset1D = glm::vec<1, int32_t>;
    using RHIOffset2D = glm::vec<2, int32_t>;
    using RHIOffset3D = glm::vec<3, int32_t>;
    // [RGBA]
    using RHIClearColor = glm::vec<4, float>;
    // [Depth Clear Value, Stencil Clear Value]
    using RHIClearDepthStencil = Pair<float, uint32_t>;

    enum class RHIResourceFormat {
        Undefined = 0,
        R8G8B8A8Unorm,
        R8G8B8A8Srgb,
        B8G8R8A8Unrom,
        B8G8R8A8Srgb,
        A2R10G10B10Unorm,
        A2R10G10B10Snorm,
        B10G11R11Ufloat,
        R32SignedFloat,
        R32G32SignedFloat,
        R32G32B32SignedFloat,
        R32G32B32A32SignedFloat,
        R16SignedFloat,
        R16G16SignedFloat,
        R16G16B16SignedFloat,
        R16G16B16A16SignedFloat,
        R32Uint,
        R16Uint,
        R16Unorm,
        D32SignedFloat,
        D16Unorm,
        Bc1RgbUnorm,
        Bc1RgbSrgb,
        Bc1RgbaUnorm,
        Bc1RgbaSrgb,
        Bc2Unorm,
        Bc2Srgb,
        Bc3Unorm,
        Bc3Srgb,
        Bc4Unorm,
        Bc4Snorm,
        Bc5Unorm,
        Bc5Snorm,
        Bc6HUfloat,
        Bc6HSfloat,
        Bc7Unorm,
        Bc7Srgb,
    };
    ENUM_NAME_CONV_BEGIN(RHIResourceFormat)
        ENUM_NAME(R8G8B8A8Unorm)
        ENUM_NAME(R8G8B8A8Srgb)
        ENUM_NAME(B8G8R8A8Unrom)
        ENUM_NAME(B8G8R8A8Srgb)
        ENUM_NAME(A2R10G10B10Unorm)
        ENUM_NAME(A2R10G10B10Snorm)
        ENUM_NAME(B10G11R11Ufloat)
        ENUM_NAME(R32SignedFloat)
        ENUM_NAME(R32G32SignedFloat)
        ENUM_NAME(R32G32B32SignedFloat)
        ENUM_NAME(R32G32B32A32SignedFloat)
        ENUM_NAME(R16SignedFloat)
        ENUM_NAME(R16G16SignedFloat)
        ENUM_NAME(R16G16B16SignedFloat)
        ENUM_NAME(R16G16B16A16SignedFloat)
        ENUM_NAME(R32Uint)
        ENUM_NAME(R16Uint)
        ENUM_NAME(D32SignedFloat)
        ENUM_NAME(Bc1RgbUnorm)
        ENUM_NAME(Bc1RgbSrgb)
        ENUM_NAME(Bc1RgbaUnorm)
        ENUM_NAME(Bc1RgbaSrgb)
        ENUM_NAME(Bc2Unorm)
        ENUM_NAME(Bc2Srgb)
        ENUM_NAME(Bc3Unorm)
        ENUM_NAME(Bc3Srgb)
        ENUM_NAME(Bc4Unorm)
        ENUM_NAME(Bc4Snorm)
        ENUM_NAME(Bc5Unorm)
        ENUM_NAME(Bc5Snorm)
        ENUM_NAME(Bc6HUfloat)
        ENUM_NAME(Bc6HSfloat)
        ENUM_NAME(Bc7Unorm)
        ENUM_NAME(Bc7Srgb)
    ENUM_NAME_CONV_END()

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
    enum class RHIDeviceQueueType : uint32_t {
        Undefined = ~0u,
        Graphics = 0,
        Compute = 1,
        Transfer = 2,
        Present = 3
    };
    ENUM_NAME_CONV_BEGIN(RHIDeviceQueueType)
        ENUM_NAME(Undefined)
        ENUM_NAME(Graphics)
        ENUM_NAME(Compute)
        ENUM_NAME(Transfer)
        ENUM_NAME(Present)
    ENUM_NAME_CONV_END()

    BITMASK_ENUM_BEGIN(RHIDeviceQueueFlags, uint32_t)
        Graphics = 1u << 0,
        Compute = 1u << 1,
        Transfer = 1u << 2,
        Present = 1u << 3
    BITMASK_ENUM_END()

    enum class RHIDevicePipelineType {
        Graphics,
        Compute,
        RayTracing
    };
    ENUM_NAME_CONV_BEGIN(RHIDevicePipelineType)
        ENUM_NAME(Graphics)
        ENUM_NAME(Compute)
        ENUM_NAME(RayTracing)
    ENUM_NAME_CONV_END()

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

    enum class RHIAccelerationStructureType
    {
        BottomLevel,
        TopLevel
    };

    enum class RHIAccelerationStructureBuildOp
    {
        Build,
        Update
    };

    enum class RHIAccelerationGeometryType
    {
        Triangles,
        Instances
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
        StorageBuffer,
        AccelerationStructure
    };

    ENUM_NAME_CONV_BEGIN(RHIDescriptorType)
        ENUM_NAME(Sampler)
        ENUM_NAME(SampledImage)
        ENUM_NAME(StorageImage)
        ENUM_NAME(UniformBuffer)
        ENUM_NAME(StorageBuffer)
        ENUM_NAME(AccelerationStructure)
    ENUM_NAME_CONV_END()

    enum class RHIMultisampleCount {
        E1, E2, E4, E8, E16
    };
    enum class RHITextureDimension {
        E1D, E2D, E3D,
        ECube, E1DArray,E2DArray, ECubeArray
    };

    BITMASK_ENUM_BEGIN(RHIShaderStage, uint32_t)
        // Vertex Shader
        Vertex = 1 << 0,
        // Fragment Shader (aka Pixel)
        Fragment = 1 << 1,
        // Compute Shader
        Compute = 1 << 2,
        // Ray Tracing Ray Generation
        RayGeneration = 1 << 3,
        // Ray Tracing Ray Any Hit
        RayAnyHit = 1 << 4,
        // Ray Tracing Ray Closest Hit
        RayClosestHit = 1 << 5,
        // Ray Tracing Ray Miss
        RayMiss = 1 << 6,
        // Ray Tracing Ray Intersection
        RayIntersection = 1 << 7,
        // Mesh Shading (aka Amplification)
        Task = 1 << 8,
        // Mesh Shading
        Mesh = 1 << 9,
        All = ~0u
    BITMASK_ENUM_END();

    ENUM_NAME_CONV_BEGIN(RHIShaderStageBits)        
        case Vertex: return "Vertex Shader";
        case Fragment: return "Fragment Shader";
        case Compute: return "Compute Shader";
        case RayGeneration: return "RT Ray Generation";
        case RayAnyHit: return "RT Any Hit";
        case RayClosestHit: return "RT Closest Hit";
        case RayMiss: return "RT Miss";
        case RayIntersection: return "RT Intersection";
        case Task: return "Task Shader";
        case Mesh: return "Mesh Shader";
    ENUM_NAME_CONV_END();
        
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
        HostWrite = 1 << 9,
        HostRead = 1 << 10,
        AccelerationStructureRead = 1 << 11,
        AccelerationStructureWrite = 1 << 12,
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
        TaskShader 			    = 1 << 7,
        RenderTargetOutput      = 1 << 8,
        Transfer                = 1 << 9,
        EarlyFragmentTests      = 1 << 10,
        LateFragmentTests       = 1 << 11,
        AccelerationBuild       = 1 << 12,
        // ---
        Host                    = 1 << 27,
        AllGraphics             = 1 << 28,
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
        TransferDestination = 1 << 6,
        DeviceAddress = 1 << 7,
        AccelerationStructureStorage = 1 << 8,
        AccelerationStructureBuildReadOnly = 1 << 9,
        ShaderBindingTable = 1 << 10
    BITMASK_ENUM_END();

    BITMASK_ENUM_BEGIN(RHITextureUsage, uint32_t)
        RenderTarget = 1 << 0,
        DepthStencil = 1 << 1,
        SampledImage = 1 << 2,
        StorageImage = 1 << 3,
        TransferSource = 1 << 4,
        TransferDestination = 1 << 5
    BITMASK_ENUM_END();

    BITMASK_ENUM_BEGIN(RHITextureAspectFlag, uint32_t)
        Color = 1 << 0,
        Depth = 1 << 1,
        Stencil = 1 << 2
    BITMASK_ENUM_END();

    BITMASK_ENUM_BEGIN(RHIAccelerationStructureBuildFlags, uint32_t)
        AllowUpdate = 1 << 0,
        AllowCompaction = 1 << 1,
        PreferFastTrace = 1 << 2,
        PreferFastBuild = 1 << 3,
        LowMemory = 1 << 4
    BITMASK_ENUM_END()

    BITMASK_ENUM_BEGIN(RHIAccelerationGeometryInstanceFlags, uint32_t)
        TriangleCullDisable = 1 << 0,
    BITMASK_ENUM_END()
}
