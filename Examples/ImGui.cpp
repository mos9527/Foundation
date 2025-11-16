#include <Bindings/ImGui.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
int main()
{
    SDL_Window* window = SDL_CreateWindow("DebugText Example", 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, {
        .threads = 0 /* ST recording */
    });
    CSDebugTextData lines[5]{};
    lines[0].x = lines[0].y = 16, lines[0].SetText("ImGui Demo Window");
    ImGui_ImplFoundation_SetupContextWithDefaultStyles();
    renderer->BeginSetup();
    ImGui_ImplFoundation_Init(device.Get(), window, renderer);
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
    renderer->EndSetup();
    SDL_Event event;
    ExampleFpsCounter fps;
    while (!Examples_ShouldClose(window, renderer, swapchain, &event))
    {
        lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
        ImGui_ImplFoundation_NewFrame(&event);
        ImGui::NewFrame();
        ImGui::ShowDemoWindow();
        Examples_NewFrame(renderer);
    }
    ImGui_ImplFoundation_Shutdown();
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
