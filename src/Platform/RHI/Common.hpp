#pragma once
#include <Core/Math.hpp>
#include <Core/Bits/Enums.hpp>
#include <Core/Allocator/StlContainers.hpp>
#include <Platform/RHI/Details/Details.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            constexpr static size_t kFullSize = -1;
            using RHIExtent1D = glm::vec<1, uint32_t>;
            using RHIExtent2D = glm::vec<2, uint32_t>;
            using RHIExtent3D = glm::vec<3, uint32_t>;
            using RHIOffset1D = glm::vec<1, int32_t>;
            using RHIOffset2D = glm::vec<2, int32_t>;
            using RHIOffset3D = glm::vec<3, int32_t>;
            using RHIClearColor = glm::vec<4, float>;

            enum class RHIResourceFormat {
                Undefined = 0,
                R8G8B8A8_UNORM,
                R32_SIGNED_FLOAT,
                R32G32_SIGNED_FLOAT,
                R32G32B32_SIGNED_FLOAT,
                R32G32B32A32_SIGNED_FLOAT,
                R32_UINT,
                R16_UINT
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

            enum class RHIImageLayout {
                Undefined = 0,
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
                // XXX: In VK there's also CombinedImageSampler though no other APIs has it.
                // See also: https://github.com/gpuweb/gpuweb/issues/770
                SampledImage,
                UniformBuffer,
                StorageBuffer
            };

            enum class RHIMultisampleCount {
                e1, e2, e4, e8, e16
            };
            enum class RHIImageDimension {
                e1D, e2D, e3D
            };

            BITMASK_ENUM_BEGIN(RHIShaderStage, uint32_t)
                Undefined = 0,
                Vertex = 1 << 0,
                Fragment = 1 << 1,
                Compute = 1 << 2,
                All = ~0u
            BITMASK_ENUM_END()

            BITMASK_ENUM_BEGIN(RHIResourceAccess, uint32_t)
                Undefined = 0,
                RenderTargetWrite = 1 << 0,
                TransferWrite = 1 << 1,
                ShaderRead = 1 << 2
            BITMASK_ENUM_END()

            BITMASK_ENUM_BEGIN(RHIPipelineStage, uint32_t)
                Undefined = 0,
                TopOfPipe = 1 << 0,
                RenderTargetOutput = 1 << 1,
                Transfer = 1 << 2,
                FragmentShader = 1 << 3,
                BottomOfPipe = 1 << 7
            BITMASK_ENUM_END()

            BITMASK_ENUM_BEGIN(RHIBufferUsage, uint32_t)
                Undefined = 0,
                VertexBuffer = 1 << 0,
                IndexBuffer = 1 << 1,
                // i.e. Uniform Buffer
                UniformBuffer = 1 << 2,
                // i.e. Structured Buffer
                StorageBuffer = 1 << 3,
                IndirectBuffer = 1 << 4,
                TransferSource = 1 << 5,
                TransferDestination = 1 << 6
            BITMASK_ENUM_END()

            BITMASK_ENUM_BEGIN(RHIImageUsage, uint32_t)
                Undefined = 0,
                RenderTarget = 1 << 0,
                DepthStencil = 1 << 1,
                SampledImage = 1 << 2,
                StorageImage = 1 << 3,
                TransferSource = 1 << 4,
                TransferDestination = 1 << 5
            BITMASK_ENUM_END()

            BITMASK_ENUM_BEGIN(RHIImageAccessFlag, uint32_t)
                Undefined = 0,
                Color = 1 << 0,
                Depth = 1 << 1,
                Stencil = 1 << 2
            BITMASK_ENUM_END();
        }
    }
}
