#include <Rendering/Application.hpp>
#include <tracy/Tracy.hpp>

#include "tracy/TracyC.h"
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
        m_alloc_renderer.Ptr(),
        RendererDesc{
            .async = m_desc.asyncCompute,
            .present = m_desc.present
        },
        m_device, m_swapchain, m_alloc_renderer.Ptr()
    );
    m_renderer->BeginSetup();
    RendererSetup();
    m_renderer->EndSetup();
}
void RenderApplication::InitializeInternal() {
    LOG_RUNTIME(RenderApplication, info, "** Application Setup **");
    LOG_RUNTIME(RenderApplication, info, "Dir: {}", std::filesystem::current_path().string());
    if (m_desc.present) {
        m_window = CreateWindow(m_desc.windowSize.x, m_desc.windowSize.y, m_desc.windowTitle.c_str());
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
        OnBeforeFrame();
        m_renderer->ExecuteFrame();
        OnAfterFrame();
        m_renderer->EndExecute();
        m_frameCondition.notify_all();
    }
    catch (RHISwapchainResizeException&) {
        CreateSwapchain();
        m_renderer->SetSwapchain(m_swapchain);
        OnSwapchainResize();
    }
}

void RenderApplication::RunForever() {
    CHECK_MSG(m_rhi, "No RHI backend initialized! Call Initialize<Backend>() first.");
    CHECK(m_device && m_renderer);
    m_state = State::Running;
    TracyCSetThreadName("Render Thread");
    do {
        Execute();
        // Update framerate
        size_t smp_tick = m_timing.begin.y;
        size_t perf_counter = getPerformanceCounter();
        if (perf_counter - smp_tick >= m_timing.kTimingSampleDuration)
        {
            m_timing.Tick({m_renderer->GetFrame(), perf_counter});
            if (m_window)
            {
                double fps = static_cast<double>(m_timing.delta.x) / (m_timing.kTimingSampleDuration / 1e9);
                m_window.SetWindowTitle(fmt::format("{} [{} FPS]", m_desc.windowTitle, fps).c_str());
            }
        }

    } while (m_window && !m_window.WindowShouldClose());
    m_state = State::Exiting;
    LOG_RUNTIME(RenderApplication, info, "Exiting gracefully.");
    m_frameCondition.notify_all();
}
