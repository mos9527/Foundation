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
     * @brief Persistent staging buffer(s) + immediate context(s) for quick, batchable uploads.
     * @note  When constructed with more than one buffer, End() advances to the next staging lane and Begin()
     *        only waits when the next lane is still in flight.
     */
    struct ImmediateUpload
    {
        struct UploadLane
        {
            ImmediateContext ctx;
            RHIDeviceScopedHandle<RHIBuffer> staging;
            char* begin;
            char* end;
            size_t signalValue{0};

            UploadLane(RHIDevice* device, size_t capacity, RHIDeviceQueueType type);
        };

        ImmediateContext ctx;
        RHIDeviceScopedHandle<RHIBuffer> staging;

        char *begin, *ptr, *end;
        ImmediateUpload(RHIDevice* device, size_t capacity,
                        RHIDeviceQueueType type = RHIDeviceQueueType::Graphics,
                        size_t buffers = 1);

        /**
         * @return Command list for the current upload lane.
         */
        [[nodiscard]] RHICommandList* Get() const;

        /**
         * Resets the upload context for a new series of uploads.
         * This MUST be called before any Upload calls.
         */
        void Begin();

        /**
         * Uploads data to `dst` buffer with a staging copy.
         * @return nullptr when upload fails (out of staging memory).
         *         At which point, a flush with End() -> Begin() is required.
         *         A mapped, writable pointer to the staging memory where the buffer data is expected to be written otherwise.
         */
        char* Upload(RHIBuffer* dst, size_t dataSize, size_t dstOffset);
        /**
         * Uploads data to `dst` texture with a staging copy.
         * @return nullptr when upload fails (out of staging memory).
         *         At which point, a flush with End() -> Begin() is required.
         *         A mapped, writable pointer to the staging memory where the texture data is expected to be written otherwise.
         */
        char* Upload(RHITexture* dst, size_t dataSize,
                     RHITextureSubresourceLayer dstLayer = {.aspect = RHITextureAspectFlagBits::Color},
                     RHIOffset2D dstOffset = {},
                     RHIExtent2D dstExtent = {});
        char* Upload(RHITexture* dst, size_t dataSize, RHITextureSubresourceLayer dstLayer, RHIOffset3D dstOffset,
                     RHIExtent3D dstExtent);

        bool Align(uint32_t alignment);
        /**
         * Finalizes the upload context, submitting the copy commands.
         * @param completionFence Optional fence to signal upon completion.
         */
        void End(RHIDeviceFence* completionFence = nullptr);
        void End(ImmediateSubmitDesc const& desc);

        void WaitIdle();

    private:
        RHIDevice* mDevice;
        size_t mLaneCount{1};
        size_t mCurrentLane{0};
        size_t mNextSignalValue{1};
        size_t mLane0SignalValue{0};
        char* mLane0Begin{nullptr};
        char* mLane0End{nullptr};
        RHIDeviceScopedHandle<RHIDeviceSemaphore> mCompletionTimeline;
        Core::Vector<Core::UniquePtr<UploadLane>> mLanes;
        Core::Vector<RHIDeviceQueue::TimelinePair> mSubmitSignals;

        [[nodiscard]] ImmediateContext& CurrentContext();
        [[nodiscard]] ImmediateContext const& CurrentContext() const;
        [[nodiscard]] RHIBuffer* CurrentStaging() const;
        [[nodiscard]] char* CurrentBegin() const;
        [[nodiscard]] char* CurrentEnd() const;
        [[nodiscard]] size_t& CurrentSignalValue();
        void WaitCurrentLaneReusable();
        void SelectCurrentLane();
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
                                                  .staging = true,
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

} // namespace Foundation::RenderCore