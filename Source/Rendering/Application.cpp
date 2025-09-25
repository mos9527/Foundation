#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

#include "Application.hpp"
using namespace Foundation::Rendering;
void RenderApplication::CreateSwapchain() {
    CHECK(m_device && m_window);
    LOG_RUNTIME(RenderApplication, info, "Creating swapchain ({}x{})", GetFramebufferSize().x, GetFramebufferSize().y);
    m_device->WaitIdle();
    m_swapchain.Reset();
    constexpr RHIResourceFormat kFormatPreferenceList[] = {
        RHIResourceFormat::R8G8B8A8_UNORM,
        RHIResourceFormat::B8G8R8A8_UNROM,
        RHIResourceFormat::R8G8B8A8_SRGB,
        RHIResourceFormat::B8G8R8A8_SRGB
    };
    constexpr RHISwapchainPresentMode kPresentModePreferenceList[] = {
        RHISwapchainPresentMode::Mailbox,
        RHISwapchainPresentMode::Tearing,
        RHISwapchainPresentMode::Fifo
    };
    auto format = Ranges::FirstOf(Views::all(kFormatPreferenceList) | Views::filter(Ranges::ContainedBy(m_device->GetSwapchainSupportedFormats())));
    auto present = Ranges::FirstOf(Views::all(kPresentModePreferenceList) | Views::filter(Ranges::ContainedBy(m_device->GetSwapchainSupportedPresentModes())));
    CHECK_MSG(format.has_value(), "No supported swapchain format found!");
    LOG_RUNTIME(RenderApplication, info, "Selected swapchain format: {}", format.value());
    CHECK_MSG(present.has_value(), "No supported presentation mode found!");
    if (m_desc.vsync)
        present = RHISwapchainPresentMode::Fifo;
    LOG_RUNTIME(RenderApplication, info, "Selected swapchain present mode: {}", present.value());
    m_swapchain = m_device->CreateSwapchain(
        RHISwapchain::SwapchainDesc{
        .format = format.value(),
        .extents = GetFramebufferSize(),
        .min_buffer_count = 3,
        .present_mode = present.value(),
    });
}
void RenderApplication::InitializeRenderer() {
    LOG_RUNTIME(RenderApplication, info, "** Renderer Setup **");
    m_renderer = ConstructUnique<Renderer>(
        m_allocRenderer.Ptr(),
        RendererDesc{
            .async = m_desc.asyncCompute,
            .present = m_desc.present
        },
        m_device, m_swapchain, m_allocRenderer.Ptr()
    );
    m_renderer->BeginSetup();
    OnRendererSetup();
    m_renderer->EndSetup();
}
void RenderApplication::InitializeInternal() {
    LOG_RUNTIME(RenderApplication, info, "** Application Setup **");
    LOG_RUNTIME(RenderApplication, info, "Dir: {}", std::filesystem::current_path().string());
    if (m_desc.present) {
        m_window = CreateNativeWindow(m_desc.windowSize.x, m_desc.windowSize.y, m_desc.windowTitle.c_str());
        m_device = m_rhi->CreateDevice(m_rhi->EnumerateDevices()[m_desc.deviceIndex], &m_window);
        CreateSwapchain();
    }
    else {
        // 'Headless' mode. No presentation therefore no swapchain/window.
        m_window = {};
        m_device = m_rhi->CreateDevice(m_rhi->EnumerateDevices()[m_desc.deviceIndex]);
    }    
    OnDeviceSetup();
}
void RenderApplication::Execute()
{
    ZoneScoped;
    try {
        m_renderer->BeginExecute();
        {
            ZoneScopedN("OnBeforeFrame");
            OnBeforeFrame();
        }
        m_renderer->ExecuteFrame();
        {
            ZoneScopedN("OnAfterFrame");
            OnAfterFrame();
        }
        m_renderer->EndExecute();
        m_renderFrame.notify_all();
    }
    catch (RHISwapchainResizeException&) {
        CreateSwapchain();
        m_renderer->SetSwapchain(m_swapchain);
        OnSwapchainResize();
        InitializeRenderer();
        Execute();
    }
}
void RenderApplication::RenderWorker()
{
    CHECK_MSG(m_rhi, "No RHI backend initialized! Call Initialize<Backend>() first.");
    CHECK(m_device && m_renderer);
    TracyCSetThreadName("Render Thread");
    while (!m_appShouldClose)
    {
        if (m_renderThreadReset.load(std::memory_order_relaxed))
        {
            m_renderThreadReset.store(false, std::memory_order_relaxed);
            LOG_RUNTIME(RenderWorker, info, "Renderer Reset");
            InitializeRenderer();
        }
        Execute();
    }
    LOG_RUNTIME(RenderWorker, info, "Render Thread exiting.");
    m_renderFrame.notify_all();
}
using namespace Foundation::Async;
void RenderApplication::RunForever() {
    m_renderThread = Thread(&RenderApplication::RenderWorker, this);
    while (!m_appShouldClose.load(std::memory_order_relaxed))
    {        
        OnApplicationTick();
        // Update framerate
        size_t smp_tick = m_timing.begin.y;
        size_t perf_counter = getPerformanceCounter();
        if (perf_counter - smp_tick >= m_timing.kTimingSampleDuration)
        {
            m_timing.Tick({m_renderer->GetFrame(), perf_counter});
            m_window.SetWindowTitle(fmt::format("{} [{} FPS]", m_desc.windowTitle, m_timing.GetFPS()).c_str());
        }
        if (m_window.WindowShouldClose())
            m_appShouldClose.store(true, std::memory_order_relaxed);
    }
    LOG_RUNTIME(RenderApplication, info, "Main Thread exiting.");
    m_renderThread.join();
    m_device->WaitIdle();
}
void RenderApplication::WaitForFrame()
{
    if (!m_appShouldClose.load(std::memory_order_relaxed))
    {
        std::unique_lock lock(m_renderMutex);
        m_renderFrame.wait(lock);
    }
}
void RenderApplication::ResetRendererOnNextFrame() { m_renderThreadReset.store(true, std::memory_order_release); }
void RenderApplication::Shutdown() { m_appShouldClose.store(true, std::memory_order_release); }
