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
     * @note Hotkeys:
     *  - Press `Space` to toggle between different shaders.
     *  - Press `C` to enter "click-through" mode.
     *  - Press `Esc` to exit the application.
     * @example Shaders/SimpleCRT.slang
     * Simple CRT effect shader
     */
    class Win32_ScreenCaptureApp : public RenderApplication
    {
        const char* kShaders[2] = {
            "data/shaders/SimpleBloom.spv",
            "data/shaders/SimpleCRT.spv"
        };
        uint32_t mShaderIndex{0};
        // Temporary staging buffer for screen capture
        UniquePtr<StagingBuffer> mStaging;
        HDC hScreenDC = NULL, hMemoryDC = NULL;
        HBITMAP hBitmap = NULL;
        struct PushConstant
        {
            float time;
            uint32_t width;
            uint32_t height;
        };
        void OnDeviceSetup() override
        {            
            mStaging = ConstructUnique<StagingBuffer>(GetRendererAllocator(), mDevice.Get(), 256_MB, GetRendererAllocator());
        }        
        void OnRendererSetup() override
        {
            GLFWwindow* window = static_cast<GLFWwindow*>(GetNativeWindow()->GetNative());
            HWND hwnd = glfwGetWin32Window(window);
            // Always on top
            glfwSetWindowAttrib(window, GLFW_FLOATING, GLFW_TRUE);            
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
                mRenderer.get(), "Screen Texture",
                RHITextureDesc{.usage = RHITextureUsageBits::TransferDestination | RHITextureUsageBits::SampledImage,
                               .extent = {w, h, 1},
                               .format = RHIResourceFormat::B8G8R8A8Unrom});
            createPass(
                mRenderer.get(), "Update Texture", RHIDeviceQueueType::Graphics, [=](PassHandle self, Renderer* r)
                {
                    r->BindTextureCopyDst(self, screen, RHITextureSubresourceRange::Create());                    
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    auto* tex = r->DerefResource(screen).Get<RHITexture*>();
                    auto* buffer = mStaging->GetBuffer();
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
                        .srcBufferOffset = 0,
                        .dstLayer =
                            {
                                .aspect = RHITextureAspectFlagBits::Color,
                                .mipLevel = 0,
                                .baseArrayLayer = 0,
                                .layerCount = 1,
                            },
                        .extent = {w, h, 1},
                    }}});
                });
            ResourceHandle sampler = mRenderer->CreateSampler({});            
            createPSFullscreenPass(
                mRenderer.get(), "Draw Screen",
                [=](PassHandle self, Renderer* r)
                {
                    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", kShaders[mShaderIndex]);
                    r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(PushConstant));
                    r->BindTextureSRV(
                        self, screen, "screen", RHIPipelineStageBits::FragmentShader,
                        {.format = RHIResourceFormat::B8G8R8A8Unrom, .range = RHITextureSubresourceRange::Create()});
                    r->BindTextureSampler(self, sampler, "sampler");
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    auto [w, h] = GetNativeWindow()->GetWindowSize();
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, PushConstant{
                        .time = GetApplicationTime<float>(),
                        .width = w,
                        .height = h
                    });
                });
        }
        void OnApplicationTick() override
        {
            GLFWwindow* window = static_cast<GLFWwindow*>(GetNativeWindow()->GetNative());
            static bool key[0xFFF]{};
            auto OnKeyDown = [&](int k) {
                if (!key[k] && glfwGetKey(window, k))
                {
                    key[k] = true;
                    return true;
                }
                if (!glfwGetKey(window, k))
                    key[k] = false;
                return false;
            };
            if (OnKeyDown(GLFW_KEY_ESCAPE))
                Shutdown();
            if (OnKeyDown(GLFW_KEY_SPACE))
            {
                mShaderIndex = (mShaderIndex + 1) % std::size(kShaders);
                LOG_RUNTIME(Win32_ScreenCaptureApp, info, "Switched to shader: {}", kShaders[mShaderIndex]);
                ResetRendererOnNextFrame();
            }
            if (OnKeyDown(GLFW_KEY_C))
            {
                glfwSetWindowAttrib(window, GLFW_MOUSE_PASSTHROUGH, GLFW_TRUE);
                glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);                
            }
        }
    };
} // namespace Examples
int main(int argc, char** argv)
{
    Examples::Win32_ScreenCaptureApp app;
    app.Initialize<VulkanApplication>({.windowTitle = "Screen Capture"});
    app.RunForever();
}
