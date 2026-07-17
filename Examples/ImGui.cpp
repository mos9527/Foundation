// Minimal Dear ImGui integration sample using the Foundation backend.
// Opens the standard ImGui demo window over a rendered frame.
#include "Examples.hpp"
#include <Bindings/ImGui.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <RenderUtils/CSClearBuffer.hpp>
#include <Renderer/Presenter.hpp>
using namespace RenderUtils;
int main(int argc, char** argv)
{
    SDL_Window* window =
        SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("ImGui Example"), 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain, presenter] = Examples_InitVulkan(window, argc, argv, {
        .threadCount = 0 /* ST recording */
    });
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("ImGui Demo Window");
    ImGui_ImplFoundation_SetupContextWithDefaultStyles();
    
    ImGui_ImplFoundation_Init(device.Get(), window);
    
    renderer->BeginSetup();
    createCSClearBackBuffer(renderer, "Clear", {0.1f, 0.1f, 0.1f, 1.0f});
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
    renderer->EndSetup();
    ExampleInputState input;
    ExampleFpsCounter fps;
    while (true)
    {
        Examples_BeginFrameInput(input);
        if (Examples_PollEvents(window, renderer, swapchain, input, nullptr, ImGui_ImplFoundation_ProcessEvent))
            break;
        lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
        ImGui_ImplFoundation_NewFrame();
        ImGui::NewFrame();
        ImGui::ShowDemoWindow();
        try
        {
            const uint32_t image = presenter->AcquireNextImage();
            renderer->BeginExecute(image, presenter->GetImageAcquireSemaphore().Get());
            renderer->ExecuteFrame();
            renderer->EndExecute();
            
            auto* swapchainPtr = swapchain.Get();
            auto* semaphore = ImGui_ImplFoundation_EndFrame(
                swapchainPtr->GetViews()[image],
                swapchainPtr->mDesc.format,
                swapchainPtr->mDesc.colorSpace,
                image,
                false /* clear */,
                renderer->GetRenderCompleteSemaphore().Get()
            );
            
            presenter->Present(semaphore);
        }
        catch (Foundation::RHI::RHISwapchainResizeException&)
        {
            LOG(Examples, LogWarn, "Swapchain invalidated; recreating presentation surface");
            try
            {
                swapchain.Reset();
                device->RefreshPresentationSurface();
                if (Examples_CreateSwapchain(window, device.Get(), swapchain))
                {
                    renderer->SetSwapchain(swapchain);
                }
            }
            catch (std::exception const& e)
            {
                LOG(Examples, LogWarn, "Swapchain recreation deferred: {}", e.what());
            }
        }
    }
    ImGui_ImplFoundation_Shutdown();
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
    return 0;
}
