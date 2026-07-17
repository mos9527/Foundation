// Minimal Dear ImGui integration sample using the Foundation backend.
// Opens the standard ImGui demo window over a rendered frame.
#include "Examples.hpp"
#include <Bindings/ImGui.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
int main(int argc, char** argv)
{
    SDL_Window* window =
        SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("ImGui Example"), 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, surface, swapchain, presenter] = Examples_InitVulkan(window, argc, argv, {
        .threadCount = 0 /* ST recording */
    });
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("ImGui Demo Window");
    ImGui_ImplFoundation_SetupContextWithDefaultStyles();
    ImGui_ImplFoundation_Init(device.Get(), window);
    renderer->BeginSetup();
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui Pass", true /* clear */, FSetupDefault{});
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
    renderer->EndSetup();
    ExampleInputState input;
    ExampleFpsCounter fps;
    while (true)
    {
        Examples_BeginFrameInput(input);
        if (Examples_PollEvents(window, renderer, surface, swapchain, input, nullptr, ImGui_ImplFoundation_ProcessEvent))
            break;
        lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
        ImGui_ImplFoundation_NewFrame();
        ImGui::NewFrame();
        ImGui::ShowDemoWindow();
        Examples_NewFrame(window, renderer, presenter, swapchain);
    }
    ImGui_ImplFoundation_Shutdown();
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
