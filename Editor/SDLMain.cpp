#include "ImGui.hpp"
#include "Paths.hpp"
#include <argh.h>
extern bool EditorProcessEvent(SDL_Event*);
extern bool EditorOnFrame(FContext*);
bool /* should close */ mainLoop()
{
    SDL_Event& event = GContext->event;
    while (SDL_PollEvent(&event))
    {
        if (event.window.windowID != SDL_GetWindowID(GContext->window))
            return false;
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            return true;
        if (event.type == SDL_EVENT_WINDOW_RESIZED)
        {
            UpdateSwapchain(GContext);
            if (GContext->renderer)
                GContext->renderer->SetSwapchain(GContext->swapchain);
        }
        if (EditorProcessEvent(&event))
            return true;
    }
    EditorOnFrame(GContext);
    return false;
}

constexpr int kSDLWindowFlagsVulkan = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
int main(int argc, char** argv)
{
    argh::parser cmdl(argc, argv, argh::parser::PREFER_PARAM_FOR_UNREG_OPTION);
    if (cmdl[{"-h", "--help"}])
    {
        fmt::println("Usage: {} [options] [files...]", argv[0]);
        fmt::println("Options:");
        fmt::println("\t-h, --help\t\tShow this help message");
        fmt::println("\t-g, --gpu <id>\t\tSpecify GPU device index");
        fmt::println("\t-l, --list-gpus\t\tList available GPU devices");
        return 0;
    }
    Paths::Init(argv[0]);

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        LOG(SDLMain, LogError, "Failed to initialize SDL: {}", SDL_GetError());
        return 1;
    }

    // --gpu / -g: specify GPU device index
    int gpuId = 0;
    cmdl({"-g", "--gpu"}, 0) >> gpuId;

    // --list-gpus: list available GPU devices and exit
    if (cmdl[{"-l", "--list-gpus"}])
    {
        auto* app = ConstructBase<RHIApplication, VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC);
        for (auto const& d : app->EnumerateDevices())
            fmt::println("[{}] {}", d.id, d.name);
        Destruct(GLOBAL_ALLOC, app);
        return 0;
    }
    SDL_Rect disp;
    SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &disp);
    CreateContext(SDL_CreateWindow("Foundation Editor", disp.w / 2, disp.h / 2, kSDLWindowFlagsVulkan),
                  GLOBAL_ALLOC, RHIDevice::DeviceDesc{.id = static_cast<uint32_t>(gpuId)});
    ImGui_ImplFoundation_SetupContextWithDefaultStyles();
    ImGui_ImplFoundation_Init(GContext->device.Get(), GContext->window);

    // Positional arguments (non-flag/option) are treated as file paths passed to the Editor
    // argh's pos_args() index 0 is the program name; file paths start from index 1
    Vector<const char*> files(GLOBAL_ALLOC);
    for (size_t i = 1; i < cmdl.pos_args().size(); ++i)
        files.push_back(cmdl.pos_args()[i].c_str());
    GContext->files = Span<const char*>(files.data(), files.data() + files.size());

    while (!mainLoop()) {}
    LOG(SDLMain, LogInfo, "Quitting...");
    ImGui_ImplFoundation_Shutdown();
    DestroyContext();
}
