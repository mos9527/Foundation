#include <Rendering/Application.hpp>
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
    RHIResourceFormat format = RHIResourceFormat::Undefined;
    auto const& supported = m_device->GetSwapchainSupportedFormats();
    Set<RHIResourceFormat> formats(supported.begin(), supported.end(), m_alloc.Ptr());
    for (auto fmt : kFormatPreferenceList) {
        if (formats.contains(fmt)) {
            format = fmt;
            LOG_RUNTIME(RenderApplication, info, "Selected swapchain format: {}", format);
            break;
        }
    }
    CHECK_MSG(format != RHIResourceFormat::Undefined, "No supported swapchain format found!");
    m_swapchain = m_device->CreateSwapchain(
        RHISwapchain::SwapchainDesc{
        .format = format,
        .extents = GetFramebufferSize(),
        .min_buffer_count = 3,
        .present_mode = RHISwapchain::SwapchainDesc::PresentMode::MAILBOX,
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
}
void RenderApplication::Execute()
{
    try {
        m_renderer->Execute();
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
    do {
        OnBeforeFrame();
        Execute();
    } while (m_window && !m_window.WindowShouldClose());
}
