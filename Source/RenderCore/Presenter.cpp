#include "Presenter.hpp"
#include <fmt/format.h>
#include <Core/Logging.hpp>
#include <tracy/Tracy.hpp>

namespace Foundation::RenderCore
{
Presenter::Presenter(RHIDevice* device, RHIDeviceHandle<RHISwapchain> swapchain, Allocator* alloc) :
    mDevice(device), mSwapchain(swapchain), mSyncs(alloc)
{
    CHECK_MSG(mSwapchain.IsValid(), "Presenter requires a valid swapchain");
    SetSwapchain(swapchain);
}

RHISwapchainResult Presenter::AcquireNextImage(uint32_t& imageIndex)
{
    ZoneScopedN("Acquire Next Image");
    const RHISwapchainResult result = mSwapchain->GetNextImage(-1, mSyncs[mCurrentSync], {}, mCurrentSwap);
    imageIndex = mCurrentSwap;
    if (RHISwapchainResultMayPresent(result))
        CHECK_MSG(mCurrentSwap < mFrameSwaps, "Invalid swapchain image index {}", mCurrentSwap);
    return result;
}

RHISwapchainResult Presenter::Present(RHIDeviceSemaphore* waitSemaphore)
{
    ZoneScopedN("Present");
    const RHISwapchainResult result = mDevice->GetDeviceQueue(RHIDeviceQueueType::Graphics)->Present({
        .imageIndex = mCurrentSwap,
        .swapchain = mSwapchain.Get(),
        .waits = {{waitSemaphore}}
    });
    mCurrentSync = (mCurrentSync + 1) % mFrameSwaps;
    return result;
}

RHIDeviceHandle<RHIDeviceSemaphore> Presenter::GetImageAcquireSemaphore() const { return mSyncs[mCurrentSync]; }

void Presenter::SetSwapchain(RHIDeviceHandle<RHISwapchain> swapchain) {
    mSwapchain = swapchain;
    mFrameSwaps = mSwapchain->GetImages().size();
    mSyncs.resize(mFrameSwaps);
    for (uint32_t i = 0; i < mFrameSwaps; ++i)
    {
        mSyncs[i] = mDevice->CreateSemaphore(false);
        mSyncs[i]->DebugSetObjectName(Format("Acquire Semaphore of Swap {}", i).c_str());
    }
    mCurrentSync = mCurrentSwap = 0;
}
} // namespace Foundation::RenderCore
