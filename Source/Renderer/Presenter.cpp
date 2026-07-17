#include "Presenter.hpp"
#include <fmt/format.h>
#include <Core/Logging.hpp>
#include <tracy/Tracy.hpp>

Presenter::Presenter(RHIDevice* device, RHIDeviceHandle<RHISwapchain> swapchain, Allocator* alloc) :
    mDevice(device), mSwapchain(swapchain), mSyncs(alloc)
{
    CHECK_MSG(mSwapchain.IsValid(), "Presenter requires a valid swapchain");
    mFrameSwaps = mSwapchain->GetImages().size();
    mSyncs.resize(mFrameSwaps);
    for (uint32_t i = 0; i < mFrameSwaps; ++i)
    {
        mSyncs[i] = mDevice->CreateSemaphore(false);
        mSyncs[i]->DebugSetObjectName(fmt::format("Acquire Semaphore of Swap {}", i).c_str());
    }
}

uint32_t Presenter::AcquireNextImage()
{
    ZoneScopedN("Acquire Next Image");
    mCurrentSwap = mSwapchain->GetNextImage(-1, mSyncs[mCurrentSync], {});
    CHECK_MSG(mCurrentSwap < mFrameSwaps, "Invalid swapchain image index {}", mCurrentSwap);
    return mCurrentSwap;
}

void Presenter::Present(RHIDeviceSemaphore* waitSemaphore)
{
    ZoneScopedN("Present");
    mDevice->GetDeviceQueue(RHIDeviceQueueType::Graphics)->Present({
        .imageIndex = mCurrentSwap,
        .swapchain = mSwapchain.Get(),
        .waits = {{waitSemaphore}}
    });

    mCurrentSync = (mCurrentSync + 1) % mFrameSwaps;
}

RHIDeviceHandle<RHIDeviceSemaphore> Presenter::GetImageAcquireSemaphore() const
{ return mSyncs[mCurrentSync]; }


