#include <Rendering/Application.hpp>
using namespace Foundation::Rendering;
void Application::CreateSwapchain() {
    CHECK(m_device && m_window);
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
void Application::InitializeInternal(ApplicationInitDesc const& desc) {
    m_window = m_app.CreateWindow(desc.windowSize.x, desc.windowSize.y, desc.windowTitle);
    m_device = m_rhi->CreateDevice(m_rhi->EnumerateDevices()[desc.deviceIndex], &m_window);
    this->CreateSwapchain();
    m_renderer = ConstructUnique<Renderer>(m_alloc_renderer.Ptr(), m_device, m_swapchain, m_alloc_renderer.Ptr());
    m_renderer->BeginSetup();
    RendererSetup();
    m_renderer->EndSetup();
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
        }
    }
}
Application::~Application() {
    if (m_device)
        m_device->WaitIdle();
}
