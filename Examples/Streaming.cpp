#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <RenderCore/Streaming.hpp>
using namespace RenderUtils;
int main()
{
    SDL_Window* window = SDL_CreateWindow("Streaming Example", 800, 600, Examples_SDLWindowFlagsVulkan);
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, {
        .threads = 0 /* ST recording */
    });
    {
        auto buf = device->CreateBuffer({
            .resource = {.heap = RHIDeviceHeapType::Local, .shared = true, .staging = false},
            .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer,
            .size = 128 * (1u << 20) /* 128 MiB */
        });
        // Lifetime of streaming pool tied to this scope
        // Ensure to destruct before device destruction
        StreamingPool stream(device.Get(), GLOBAL_ALLOC, {
            .maxTransferPerSubmit = 1 /* For demonstration - don't do this. You want a reasonably large batch size. */
        });
        CSDebugTextData lines[5];
        lines[0].x = lines[0].y = 16, lines[0].SetText("Streaming Example");
        renderer->BeginSetup();
        createCSClearBackBuffer(renderer, "Clear");
        createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
        renderer->EndSetup();
        ExampleFpsCounter fps;
        auto data = Vector<char>(128LL * (1u << 20), GLOBAL_ALLOC); // 128 MiB of data
        SDL_Event event;
        while (!Examples_ShouldClose(window, renderer, swapchain, &event))
        {
            lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format("FPS: {}", fps.Update()));
            lines[2].x = 16, lines[2].y = 64, lines[2].SetText(fmt::format("{}", stream.DbgGetStatistics()));
            Examples_NewFrame(renderer);
            // SDL3 Get Enter Pressed?
            if (event.type == SDL_EVENT_KEY_UP)
            {
                if (event.key.scancode == SDL_SCANCODE_RETURN)
                {
                    // Simulate streaming more data
                    LOG_RUNTIME(Example, LogDebug, "Write");
                    stream.Write(data, buf.Get(), 0);
                }
                if (event.key.scancode == SDL_SCANCODE_SPACE)
                {
                    LOG_RUNTIME(Example, LogDebug, "Clean");
                    stream.Reset();
                }
            }
        }
    }
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
