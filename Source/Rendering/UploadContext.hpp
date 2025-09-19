#pragma once
#include <Bits/Format.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/Resource.hpp>
#include "StagingBuffer.hpp"
namespace Foundation::Rendering
{
    using namespace RHI;
    /**
     * @brief Immediate upload context for transferring data to GPU resources.
     *
     * @note All uploads are submitted to the _Graphics_ Queue, and are executed immediately.
     *       Therefore, access on the same resources on the Graphics is safe and well-defined since
     *       queue submissions are inherently 'single-threaded'.
     * @note If resources are to be accessed on the Async Compute queue, however - beware of synchronization.
     * @note Upload operations are asynchronous. And is only guaranteed to be available after a @ref UploadContext::WaitAll() call.
     *       The resources will be transitioned to-and-from states that's suitable
     * @note This is RAII - and is encouraged to be used as such. The destructor will wait for all uploads to complete.
     */
    class UploadContext
    {
        RHIDevice* m_device;
        Allocator* m_allocator;

        RHIDeviceQueue* m_queue;
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