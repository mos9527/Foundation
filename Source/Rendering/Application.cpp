#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

#include "Application.hpp"
using namespace Foundation::Rendering;
constexpr RHIResourceFormat kFormatPreferenceList[] = {
    RHIResourceFormat::R8G8B8A8Unorm,
    RHIResourceFormat::B8G8R8A8Unrom,
    RHIResourceFormat::R8G8B8A8Srgb,
    RHIResourceFormat::B8G8R8A8Srgb
};
constexpr RHISwapchainPresentMode kPresentModePreferenceList[] = {
    RHISwapchainPresentMode::Mailbox,
    RHISwapchainPresentMode::Tearing,
    RHISwapchainPresentMode::Fifo
};
void RenderApplication::CreateSwapchain() {
    CHECK(mDevice && mWindow);
    LOG_RUNTIME(RenderApplication, info, "Creating swapchain ({}x{})", GetFramebufferSize().x, GetFramebufferSize().y);
    mDevice->WaitIdle();
    mSwapchain.Reset();
    auto format = Ranges::FirstOf(Views::all(kFormatPreferenceList) | Views::filter(Ranges::ContainedBy(mDevice->GetSwapchainSupportedFormats())));
    auto present = Ranges::FirstOf(Views::all(kPresentModePreferenceList) | Views::filter(Ranges::ContainedBy(mDevice->GetSwapchainSupportedPresentModes())));
    CHECK_MSG(format.has_value(), "No supported swapchain format found!");
    LOG_RUNTIME(RenderApplication, info, "Selected swapchain format: {}", format.value());
    CHECK_MSG(present.has_value(), "No supported presentation mode found!");
    if (mDesc.vsync)
        present = RHISwapchainPresentMode::Fifo;
    LOG_RUNTIME(RenderApplication, info, "Selected swapchain present mode: {}", present.value());
    mSwapchain = mDevice->CreateSwapchain(
        RHISwapchain::SwapchainDesc{
        .format = format.value(),
        .extents = GetFramebufferSize(),
        .minBufferCount = 3,
        .presentMode = present.value(),
    });
}
void RenderApplication::InitializeRenderer() {
    LOG_RUNTIME(RenderApplication, info, "** Renderer Setup **");
    mRenderer.reset();
    mRenderer = ConstructUnique<Renderer>(
        mAllocRenderer.Ptr(),
        RendererDesc{
            .async = mDesc.asyncCompute,
            .present = mDesc.present
        },
        mDevice, mSwapchain, mAllocRenderer.Ptr()
    );
    mRenderer->BeginSetup();
    OnRendererSetup();
    mRenderer->EndSetup();
    OnRendererPostSetup();
}
void RenderApplication::InitializeInternal() {
    LOG_RUNTIME(RenderApplication, info, "** Application Setup **");
    LOG_RUNTIME(RenderApplication, info, "Dir: {}", std::filesystem::current_path());
    if (mDesc.present) {
        mWindow = CreateNativeWindow(mDesc.windowSize.x, mDesc.windowSize.y, mDesc.windowTitle.c_str());
        mDevice = mRHI->CreateDevice(mRHI->EnumerateDevices()[mDesc.deviceIndex], &mWindow);
        CreateSwapchain();
    }
    else {
        // 'Headless' mode. No presentation therefore no swapchain/window.
        mWindow = {};
        mDevice = mRHI->CreateDevice(mRHI->EnumerateDevices()[mDesc.deviceIndex]);
    }    
    OnDeviceSetup();
}
void RenderApplication::Execute()
{
    ZoneScoped;
    mRenderer->BeginExecute();
    {
        ZoneScopedN("OnBeforeFrame");
        OnBeforeFrame();
    }
    mRenderer->ExecuteFrame();
    {
        ZoneScopedN("OnAfterFrame");
        OnAfterFrame();
    }
    mRenderer->EndExecute();
    mRenderFrame.notify_all();
}
void RenderApplication::RenderWorker()
{
    TracyCSetThreadName("Render Thread"); 
    CHECK_MSG(mRHI, "No RHI backend initialized! Call Initialize<Backend>() first.");
    InitializeInternal();
    InitializeRenderer();
    CHECK(mDevice && mRenderer);       
    mRenderThreadStarted = true;
    mRenderFrame.notify_all(); // Wake up waiting thread to start their ticks.
    RHIExtent2D frameBufferSize = GetFramebufferSize();
    while (!((mAppShouldClose = mWindow.WindowShouldClose())))
    {
        // Reset swapchain if we need to
        // TODO: There could be more ways for Present to fail - we're only capturing resized windows for now
        {
            RHIExtent2D swapchainSize = GetFramebufferSize();
            if (frameBufferSize != swapchainSize)
            {
                CreateSwapchain();
                if (mDesc.initOnResize)
                    InitializeRenderer();
                else
                    mRenderer->SetSwapchain(mSwapchain);
                frameBufferSize = swapchainSize;
            }
        }
        if (mRenderThreadReset)
        {
            mRenderThreadReset = false;
            LOG_RUNTIME(RenderWorker, info, "Renderer Reset");
            InitializeRenderer();
        }
        try
        {
            Execute();
        } catch (RHISwapchainResizeException const&)
        {
            LOG_RUNTIME(RenderWorker, critical, "Swapchain resize failure!");
            throw;
        }
    }
    LOG_RUNTIME(RenderWorker, info, "Render Thread exiting.");
    mRenderFrame.notify_all();
}
using namespace Foundation::Async;
void RenderApplication::RunForever() {
    mRenderThread = Thread(&RenderApplication::RenderWorker, this);
    WaitForFrame(); // Wait for the render thread to finish initialization.
    while (!mAppShouldClose)
    {        
        OnApplicationTick();
        // Update framerate
        size_t smp_tick = mTiming.begin.y;
        size_t perf_counter = getPerformanceCounter();
        if (perf_counter - smp_tick >= mTiming.kTimingSampleDuration)
        {
            mTiming.Tick({mRenderer->GetFrame(), perf_counter});
            mWindow.SetWindowTitle(fmt::format("{} [{} FPS]", mDesc.windowTitle, mTiming.GetFPS()).c_str());
        }                 
    }
    LOG_RUNTIME(RenderApplication, info, "Main Thread exiting.");
    mRenderThread.join();
    mDevice->WaitIdle();
}
void RenderApplication::WaitForFrame()
{    
    std::unique_lock lock(mRenderMutex);
    mRenderFrame.wait(lock, [&]() { return mRenderThreadStarted; });
}
void RenderApplication::ResetRendererOnNextFrame() { mRenderThreadReset = true; }
void RenderApplication::Shutdown() { mAppShouldClose = true; }
