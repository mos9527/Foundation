#pragma once
#include <RHICore/Resource.hpp>
#include <RHICore/Device.hpp>
#include <Bits/Format.hpp>
#include "Staging.hpp"
namespace Foundation::Rendering
{
    using namespace RHI;
    /**
     * @brief Immediate upload context for transferring data to GPU resources.
     *
     * @note All uploads are submitted to a transfer queue, and are executed immediately.
     * A fence is returned that can be waited on to ensure the upload is complete.
     *
     * @note This is RAII - and is encouraged to be used as such. The destructor will wait for all uploads to complete.
     */
    class UploadContext
    {
        RHIDevice* m_device;
        Allocator* m_allocator;

        RHIDeviceQueue* m_transferQueue;
        RHIDeviceScopedObjectHandle<RHICommandPool> m_commandPool;
        Vector<RHICommandPoolScopedHandle<RHICommandList>> m_commandLists;
        Vector<RHIDeviceScopedObjectHandle<RHIDeviceFence>> m_fences;
        StagingBuffer m_stagingBuffer;
    public:
        UploadContext(RHIDevice* device, Allocator* allocator, size_t stagingBudget = 16_MB);
        RHIDeviceFence* Upload(RHIBuffer*  dst, Span<const char> data, size_t dstOffset = 0, size_t alignment = 4);
        RHIDeviceFence* Upload(RHITexture* dst, Span<const char> data, uint32_t mipLevel = 0, uint32_t arrayLayer = 0, RHITextureAspectFlag aspect = RHITextureAspectFlagBits::Color);
        void WaitAll();
        ~UploadContext();
    };
}