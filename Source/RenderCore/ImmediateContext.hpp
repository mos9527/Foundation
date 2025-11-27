#pragma once
#include <RHICore/Resource.hpp>
#include <RHICore/Command.hpp>
#include <Core/Variant.hpp>
#include <Core/AtomicPool.hpp>
namespace Foundation::RenderCore
{
    using namespace RHI;
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

        RHICommandList* Get() const { return mCommandList.Get(); }
        RHICommandList* operator->() { return mCommandList.Get(); }

        void Submit(RHIDeviceFence* completionFence = nullptr);

        void WaitIdle();
    };
}