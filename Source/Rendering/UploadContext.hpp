#pragma once
#include <Async/Future.hpp>
#include <Bits/Format.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/Resource.hpp>
#include "StagingBuffer.hpp"
namespace Foundation::Rendering
{
    using namespace RHI;
    /**
     * @brief Deferred upload context for transferring data to GPU resources.
     *
     * @note This is thread-safe, and can be used from multiple threads.
     * @note This is intended for one-off uploads, and not for streaming data.
     *       For streaming data, consider using a @ref StagingBuffer for data that
     *       might be modified often e.g. every frame.
     * @note All uploads are submitted to the _Graphics_ Queue, and are only executed when
     *       @ref Submit() is called.
     * @note This is RAII - and is encouraged to be used as such. The destructor will
     * flush all pending upload and wait for all uploads to complete.
     *
     * @param device The RHI device to use for uploads.
     * @param allocator The allocator to use for internal allocations.
     * @param stagingBudget The size of the internal staging buffer to use for uploads.
     */
    class UploadContext
    {
        RHIDevice* mDevice;
        Allocator* mAllocator;

        RHIDeviceQueue* mQueue;
        RHIDeviceScopedObjectHandle<RHICommandPool> mCommandPool;
        Vector<RHICommandPoolScopedHandle<RHICommandList>> mCommandLists;
        RHIDeviceScopedObjectHandle<RHIDeviceFence> mFence;
        StagingBuffer mStagingBuffer;

        Async::Mutex mMutex;
    public:
        UploadContext(RHIDevice* device, Allocator* allocator, size_t stagingBudget = 16_MB);
        // !! TODO: Bound checks!
        void Upload(RHIBuffer* dst, Span<const char> data, size_t dstOffset = 0, size_t alignment = 4,
                               RHIResourceAccess dst_access = RHIResourceAccessBits::ShaderRead,
                               RHIPipelineStage dst_stage = RHIPipelineStageBits::AllGraphics);
        void Upload(RHITexture* dst, Span<const char> data, uint32_t mipLevel = 0, uint32_t arrayLayer = 0,
                               RHITextureAspectFlag aspect = RHITextureAspectFlagBits::Color,
                               RHIResourceAccess dst_access = RHIResourceAccessBits::ShaderRead,
                               RHIPipelineStage dst_stage = RHIPipelineStageBits::AllGraphics,
                               RHITextureLayout dst_layout = RHITextureLayout::ShaderReadOnly);
        void SubmitAndWait();
        ~UploadContext();
    };
} // namespace Foundation::Rendering
