#pragma once
#include <Core/Container.hpp>
#include <RHICore/Command.hpp>
#include <RHICore/Device.hpp>
#include <RHICore/Resource.hpp>
namespace Foundation::RenderCore
{
    using namespace RHI;

    struct ImmediateSubmitDesc
    {
        Span<const RHIDeviceQueue::TimelinePair> timelineWaits{};
        Span<const RHIDeviceQueue::TimelinePair> timelineSignals{};
        Span<const RHIPipelineStage> waitStages{};
        RHIDeviceFence* completionFence{nullptr};
    };

    /**
     * @brief Single persistent command list for immediate submissions.
     * @note  This is primarily intended for quick one-shot commands, e.g. resource transitions, copies, etc.
     *        Avoid using this in hot path, or at all, if you could - which is usually the case.
     *        This has only been sparingly used in Examples so far.
     */
    class ImmediateContext
    {
        RHIDevice* const mDevice;
        RHIDeviceQueue* mQueue;

        RHIDeviceScopedHandle<RHICommandPool> mCommandPool;
        RHICommandPoolScopedHandle<RHICommandList> mCommandList; // Persistent
    public:
        ImmediateContext(RHIDeviceQueueType type, RHIDevice* device);
        ImmediateContext(RHIDevice* device) : ImmediateContext(RHIDeviceQueueType::Graphics, device) {}

        [[nodiscard]] RHICommandList* Get() const { return mCommandList.Get(); }
        RHICommandList* operator->() { return mCommandList.Get(); }

        void Submit(RHIDeviceFence* completionFence = nullptr);
        void Submit(ImmediateSubmitDesc const& desc);

        void WaitIdle();
    };

    /**
     * @brief Persistent staging lanes + immediate contexts for batchable uploads.
     * @note  Acquire an exclusive @ref UploadBatch with @ref BeginBatch / @ref TryBeginBatch.
     *        Each batch owns one lane until @ref UploadBatch::End, after which the lane stays
     *        in-flight until its completion timeline value is reached.
     */
    struct ImmediateUpload
    {
        class UploadBatch
        {
            friend struct ImmediateUpload;
            ImmediateUpload* mOwner{};
            size_t mLane{SIZE_MAX};
            size_t mCompletionValue{};

            UploadBatch(ImmediateUpload* owner, size_t lane) noexcept;

        public:
            char *begin{}, *ptr{}, *end{};

            UploadBatch() = default;
            UploadBatch(UploadBatch const&) = delete;
            UploadBatch& operator=(UploadBatch const&) = delete;
            UploadBatch(UploadBatch&& other) noexcept;
            UploadBatch& operator=(UploadBatch&& other) noexcept;
            ~UploadBatch() noexcept;

            [[nodiscard]] bool IsValid() const noexcept { return mOwner != nullptr; }
            [[nodiscard]] RHICommandList* Get() const;
            [[nodiscard]] size_t CompletionValue() const noexcept { return mCompletionValue; }
            [[nodiscard]] RHIDeviceSemaphore* CompletionTimeline() const;

            char* Upload(RHIBuffer* dst, size_t dataSize, size_t dstOffset);
            char* Upload(RHITexture* dst, size_t dataSize,
                         RHITextureSubresourceLayer dstLayer = {.aspect = RHITextureAspectFlagBits::Color},
                         RHIOffset2D dstOffset = {}, RHIExtent2D dstExtent = {});
            char* Upload(RHITexture* dst, size_t dataSize, RHITextureSubresourceLayer dstLayer, RHIOffset3D dstOffset,
                         RHIExtent3D dstExtent);
            bool Align(uint32_t alignment);
            void End(RHIDeviceFence* completionFence = nullptr);
            void End(ImmediateSubmitDesc const& desc);
        };

        ImmediateUpload(RHIDevice* device, size_t capacity, RHIDeviceQueueType type = RHIDeviceQueueType::Graphics,
                        size_t buffers = 1);

        [[nodiscard]] size_t Capacity() const noexcept { return mCapacity; }
        [[nodiscard]] size_t LaneCount() const noexcept { return mLanes.size(); }
        [[nodiscard]] RHIDeviceSemaphore* CompletionTimeline() const { return mCompletionTimeline.Get(); }

        UploadBatch BeginBatch();
        [[nodiscard]] bool TryBeginBatch(UploadBatch& out);
        [[nodiscard]] bool WaitTimeline(size_t value, size_t timeout) const;
        void WaitIdle();

    private:
        struct UploadLane
        {
            ImmediateContext ctx;
            RHIDeviceScopedHandle<RHIBuffer> staging;
            char* begin{};
            char* end{};
            size_t signalValue{};
            bool recording{};

            UploadLane(RHIDevice* device, size_t capacity, RHIDeviceQueueType type);
        };

        RHIDevice* mDevice;
        size_t mCapacity{};
        size_t mNextLane{};
        size_t mNextSignalValue{1};
        RHIDeviceScopedHandle<RHIDeviceSemaphore> mCompletionTimeline;
        Core::Vector<Core::UniquePtr<UploadLane>> mLanes;
        Core::Vector<RHIDeviceQueue::TimelinePair> mSubmitSignals;

        [[nodiscard]] bool IsLaneReusable(size_t lane, size_t timeout);
        void WaitLaneReusable(size_t lane);
        [[nodiscard]] bool TryAcquireLane(size_t lane, UploadBatch& out);
        UploadBatch AcquireLane(size_t lane);
        void ReleaseRecording(size_t lane) noexcept;
    };

    /**
     * @brief Single persistent staging buffer + immediate context for quick, batchable readbacks.
     */
    struct ImmediateReadback
    {
        ImmediateContext ctx;
        RHIDeviceScopedHandle<RHIBuffer> staging;

        char *begin, *ptr, *end;
        ImmediateReadback(RHIDevice* device, size_t capacity, RHIDeviceQueueType type = RHIDeviceQueueType::Graphics) :
            ctx(type, device),
            staging(device->CreateBuffer({.resource =
                                              {
                                                  .heap = RHIDeviceHeapType::Readback,
                                                  .hostAccess = RHIResourceHostAccess::ReadWrite,
                                                  .shared = false, /* Transfer only */
                                                  .coherent = true, /* No invalidate required */
                                                  .staging = false,
                                              },
                                          .usage = RHIBufferUsageBits::TransferDestination,
                                          .size = capacity}))
        {
            begin = ptr = staging->Map<char>();
            end = ptr + capacity;
        }

        /**
         * Resets the readback context for a new series of readbacks.
         * This MUST be called before any Readback calls.
         */
        void Begin();

        /**
         * Reads data from `src` buffer with a staging copy.
         * @return nullptr when readback fails (out of staging memory).
         *         At which point, a flush with End() -> WaitIdle() -> Begin() is required.
         *         A mapped, readable pointer to the staging memory where the buffer data will be available after End() and WaitIdle().
         */
        char* Readback(RHIBuffer* src, size_t dataSize, size_t srcOffset = 0);
        /**
         * Reads data from `src` texture with a staging copy.
         * @return nullptr when readback fails (out of staging memory).
         *         At which point, a flush with End() -> WaitIdle() -> Begin() is required.
         *         A mapped, readable pointer to the staging memory where the texture data will be available after End() and WaitIdle().
         */
        char* Readback(RHITexture* src, size_t dataSize,
                       RHITextureSubresourceLayer srcLayer = {.aspect = RHITextureAspectFlagBits::Color},
                       RHIOffset2D srcOffset = {},
                       RHIExtent2D srcExtent = {});
        char* Readback(RHITexture* src, size_t dataSize, RHITextureSubresourceLayer srcLayer, RHIOffset3D srcOffset,
                       RHIExtent3D srcExtent);

        bool Align(uint32_t alignment);
        /**
         * Finalizes the readback context, submitting the copy commands.
         * @param completionFence Optional fence to signal upon completion.
         */
        void End(RHIDeviceFence* completionFence = nullptr);
        void End(ImmediateSubmitDesc const& desc);

        void WaitIdle();
    };

    /**
     * @brief Creates a buffer and uploads `data` into it in a single shot.
     * @return The created buffer. No layout transition is performed (none is meaningful for buffers).
     */
    RHIDeviceScopedHandle<RHIBuffer> ImmediateCreateBuffer(RHIDevice* device, RHIBufferDesc const& desc,
                                                           void const* data, size_t bytes);

    /**
     * @brief Creates a texture and uploads `data` (base mip) into it in a single shot.
     * @param finalLayout Layout to leave the texture in after the upload. Defaults to TransferDst
     *        (matching ImmediateUpload::Upload); pass ShaderReadOnly for a texture you intend to sample.
     * @return The created texture.
     */
    RHIDeviceScopedHandle<RHITexture> ImmediateCreateTexture(RHIDevice* device, RHITextureDesc const& desc,
                                                           void const* data, size_t bytes,
                                                           RHITextureLayout finalLayout = RHITextureLayout::TransferDst);

} // namespace Foundation::RenderCore
