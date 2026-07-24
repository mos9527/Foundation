#pragma once
#include <RHICore/Swapchain.hpp>
#include <chrono>
#include <vector>

using namespace Foundation;
using namespace RHI;

namespace Foundation::RenderCore
{
class Presenter
{
public:
    Presenter(RHIDevice* device, RHIDeviceHandle<RHISwapchain> swapchain, Allocator* alloc);
    ~Presenter() = default;

    /**
     * @brief Acquires the next swapchain image.
     * @return Swapchain result; !RHISwapchainResultMayPresent means the owner should recreate before retrying.
     */
    RHISwapchainResult AcquireNextImage(uint32_t& imageIndex);

    /**
     * @brief Presents the most recently acquired image to the graphics queue.
     * @return Swapchain result; !RHISwapchainResultMayPresent means the owner should recreate.
     * @note Advances the internal frame-in-flight sync slot even on failure.
     */
    RHISwapchainResult Present(RHIDeviceSemaphore* waitSemaphore);

    /**
     * @brief Binary semaphore to signal from @ref RHISwapchain::GetNextImage for the next frame.
     */
    [[nodiscard]] RHIDeviceHandle<RHIDeviceSemaphore> GetImageAcquireSemaphore() const;

    /**
     * @brief Returns the currently used @ref RHISwapchain object.
     */
    RHIDeviceHandle<RHISwapchain> GetSwapchain() const { return mSwapchain; }

    /**
    * @breif Updates the current swapchain to the new one, resetting sync primitives.
    */
    void SetSwapchain(RHIDeviceHandle<RHISwapchain> swapchain);

private:
    RHIDevice* mDevice;
    RHIDeviceHandle<RHISwapchain> mSwapchain;    
    Vector<RHIDeviceScopedHandle<RHIDeviceSemaphore>> mSyncs;

    uint32_t mCurrentSync = 0;
    uint32_t mCurrentSwap = 0;
    uint32_t mFrameSwaps = 0;
};
} // namespace Foundation::RenderCore
