#pragma once
#include <array>
#include <Core/Bits/Enums.hpp>
#include <Core/Allocator/StlContainers.hpp>
#include <Platform/RHI/Details/Details.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            enum class RHIResourceFormat {
                Undefined = 0,
                R8G8B8A8_UNORM
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
                Present
            };
            BITMASK_ENUM_BEGIN(RHIResourceAccess, uint32_t)
                Undefined = 0,
                RenderTargetWrite = 1 << 0,
            BITMASK_ENUM_END()

            BITMASK_ENUM_BEGIN(RHIPipelineStage, uint32_t)
                Undefined = 0,
                TopOfPipe = 1 << 0,
                RenderTargetOutput = 1 << 1,
                BottomOfPipe = 1 << 7
            BITMASK_ENUM_END()

            BITMASK_ENUM_BEGIN(RHIBufferUsage, uint32_t)
                Undefined = 0,
                VertexBuffer = 1 << 0,
                IndexBuffer = 1 << 1,
                ConstantBuffer = 1 << 2,
                StorageBuffer = 1 << 3,
                IndirectBuffer = 1 << 4,
                TransferSource = 1 << 5,
                TransferDestination = 1 << 6
            BITMASK_ENUM_END()

            struct RHIClearColor {
                float r, g, b, a;

                inline constexpr std::array<float, 4> ToArray() const {
                    return { r,g,b,a };
                }
            };
        }
    }
}
