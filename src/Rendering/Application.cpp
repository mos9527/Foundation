#include <Rendering/Application.hpp>
using namespace Foundation::Rendering;
void Application::CreateSwapchain() {
    CHECK(m_device && m_window);
    LOG_RUNTIME(Application, info, "Creating swapchain ({}x{})", GetWindowSize().x, GetWindowSize().y);
    m_device->WaitIdle();
    m_swapchain.Reset();
    m_swapchain = m_device->CreateSwapchain(
        RHISwapchain::SwapchainDesc{
        .format = RHIResourceFormat::R8G8B8A8_UNORM,
        .extents = GetWindowSize(),
        .buffer_count = 3,
        .present_mode = RHISwapchain::SwapchainDesc::PresentMode::MAILBOX,
    });
}
void Application::InitializeRenderer() {
    LOG_RUNTIME(Application, info, "** Renderer Setup **");
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
void Application::InitializeInternal() {
    LOG_RUNTIME(Application, info, "** Application Setup **");
    LOG_RUNTIME(Application, info, "Dir: {}", std::filesystem::current_path().string());
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
}
void Application::RunForever() {
    CHECK_MSG(m_rhi, "No RHI backend initialized! Call Initialize<Backend>() first.");
    CHECK(m_device && m_window && m_swapchain && m_renderer);
    while (!m_window.WindowShouldClose()) {
        try {
            m_renderer->Execute();
        }
        catch (RHISwapchainResizeException&) {
            CreateSwapchain();
            m_renderer->SetSwapchain(m_swapchain);
            OnSwapchainResize();
        }
    }
}
Application::~Application() {
    if (m_device)
        m_device->WaitIdle();
}
