#include <Rendering/UploadContext.hpp>
#include <Rendering/StagingBuffer.hpp>
#include "Examples.hpp"

#define WIN32_LEAN_AND_MEAN
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
namespace Examples
{
    /**
     * @example Win32_ScreenCaptureApp.cpp
     * Capture screen content on Windows using GDI and display it using a fullscreen quad.
     * @example Shaders/SimpleCRT.slang
     * Simple CRT effect shader
     */
    class Win32_ScreenCaptureApp : public RenderApplication
    {
        // Temporary staging buffer for screen capture
        UniquePtr<StagingBuffer> m_staging;
        HDC hScreenDC = NULL, hMemoryDC = NULL;
        HBITMAP hBitmap = NULL;
        void OnDeviceSetup() override
        {            
            m_staging = ConstructUnique<StagingBuffer>(GetRendererAllocator(), m_device.Get(), 256_MB, GetRendererAllocator());
        }
        void OnRendererSetup() override
        {
            HWND hwnd = glfwGetWin32Window(static_cast<GLFWwindow*>(GetNativeWindow()->GetNative()));
            // Exclude this window from any screen capture
            SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
            auto [w, h] = GetNativeWindow()->GetWindowSize();
            if (hBitmap)
                DeleteObject(hBitmap);
            if (hMemoryDC)
                DeleteDC(hMemoryDC);
            hScreenDC = GetDC(NULL);
            hMemoryDC = CreateCompatibleDC(hScreenDC);
            hBitmap = CreateCompatibleBitmap(hScreenDC, w, h);
            SelectObject(hMemoryDC, hBitmap);
            ResourceHandle screen = createResource(
                m_renderer.get(), "Screen Texture",
                RHITextureDesc{.usage = RHITextureUsageBits::TransferDestination | RHITextureUsageBits::SampledImage,
                               .extent = {w, h, 1},
                               .format = RHIResourceFormat::B8G8R8A8_UNROM});
            createPass(
                m_renderer.get(), "Update Texture", RHIDeviceQueueType::Graphics, [=](PassHandle self, Renderer* r)
                { r->BindTextureCopyDst(self, screen, RHITextureSubresourceRange::Create()); },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    auto* tex = r->DerefResource(screen).Get<RHITexture*>();
                    auto* buffer = m_staging->GetBuffer();
                    auto* mapped = buffer->Map();
                    // Capture the screen using GDI
                    // BitBlt is slow and usually can't get 60FPS, but it's the simplest way to do this
                    {
                        POINT clientTopLeft = { 0, 0 };
                        ClientToScreen(hwnd, &clientTopLeft);
                        BitBlt(hMemoryDC, 0, 0, w, h, hScreenDC, clientTopLeft.x, clientTopLeft.y, SRCCOPY | CAPTUREBLT);
                        BITMAPINFOHEADER bi = {};
                        bi.biSize = sizeof(BITMAPINFOHEADER);
                        bi.biWidth = w,
                        bi.biHeight = -h; // Negative for top-down bitmap
                        bi.biPlanes = 1;
                        bi.biBitCount = 32;
                        bi.biCompression = BI_RGB;
                        GetDIBits(hMemoryDC, hBitmap, 0, h, mapped, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
                    }
                    buffer->Flush();
                    cmd->CopyBufferToImage(buffer, tex, RHITextureLayout::TransferDst, {{{
                        .src_buffer_offset = 0,
                        .dst_layer =
                            {
                                .aspect = RHITextureAspectFlagBits::Color,
                                .mip_level = 0,
                                .base_array_layer = 0,
                                .layer_count = 1,
                            },
                        .extent = {w, h, 1},
                    }}});
                });
            ResourceHandle sampler = m_renderer->CreateSampler({});
            createPSFullscreenPass(
                m_renderer.get(), "Draw Screen",
                [=](PassHandle self, Renderer* r)
                {
                    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/SimpleCRT.spv");
                    r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float));
                    r->BindTextureSRV(
                        self, screen, "screen", RHIPipelineStageBits::FragmentShader,
                        {.format = RHIResourceFormat::B8G8R8A8_UNROM, .range = RHITextureSubresourceRange::Create()});
                    r->BindTextureSampler(self, sampler, "sampler");
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, GetApplicationTime());
                });
        }
    };
} // namespace Examples
int main(int argc, char** argv)
{
    Examples::Win32_ScreenCaptureApp app;
    app.Initialize<VulkanApplication>({.windowTitle = "Screen Capture", .asyncCompute = false, .vsync = true });
    app.RunForever();
}
