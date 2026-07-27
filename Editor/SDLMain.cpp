#include "ImGui.hpp"
#include "Paths.hpp"
#include "EditorState.hpp"
#include <Core/BuildInfo.hpp>
#include <argh.h>
#include <algorithm>
extern bool EditorProcessEvent(SDL_Event*);
extern bool EditorOnFrame(FContext*);
extern void EditorCleanup();

bool /* should close */ mainLoop()
{
    SDL_Event& event = GContext->event;
    while (SDL_PollEvent(&event))
    {
        if (event.window.windowID != SDL_GetWindowID(GContext->window))
            return false;
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            return true;
        bool updateSwapchain = event.type == SDL_EVENT_WINDOW_RESIZED;
        if (event.type == SDL_EVENT_WINDOW_HDR_STATE_CHANGED)
        {
            UpdateWindowHDRState(GContext);
            updateSwapchain = true;
        }
        if (updateSwapchain)
        {
            UpdateSwapchain(GContext);
            if (GContext->renderer)
                GContext->renderer->SetSwapchain(GContext->swapchain);
            if (GContext->presenter)
                GContext->presenter->SetSwapchain(GContext->swapchain);
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
        Println("Usage: {} [options] [files...]", argv[0]);
        Println("Options:");
        Println("\t-h, --help\t\tShow this help message");
        Println("\t-g, --gpu <id>\t\tSpecify GPU device index");
        Println("\t-l, --list-gpus\t\tList available GPU devices");
        Println("\t-s, --render-scale <f>\tInitial render resolution scale (0.25..1.0)");
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
            Println("[{}] {}", d.id, d.name);
        Destruct(GLOBAL_ALLOC, app);
        return 0;
    }
    SDL_Rect disp;
    SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &disp);
    CreateContext(SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("Foundation Editor"), disp.w / 2, disp.h / 2,
                                   kSDLWindowFlagsVulkan),
                  GLOBAL_ALLOC, RHIDevice::DeviceDesc{.id = static_cast<uint32_t>(gpuId)});
    ImGui_ImplFoundation_SetupContextWithDefaultStyles();
    ImGui_ImplFoundation_Init(GContext->device.Get(), GContext->window);

    // Renderer Settings
    // --energy-clamp / -e: specify energy clamp value
    cmdl({"-e", "--energy-clamp"}, 2.0f) >> GContext->rendererSettings.energyClampOverride;
    // --renderer / -r: specify default renderer (0: Progressive PT, 1: RASTER, 2: Realtime PT / RTPT)
    cmdl({"-r", "--renderer"}, 0) >> GContext->rendererSettings.defaultRenderer;
    // --render-scale / -s: initial viewport render resolution scale
    cmdl({"-s", "--render-scale"}, 1.0f) >> GContext->rendererSettings.renderScale;
    GContext->rendererSettings.renderScale = std::clamp(GContext->rendererSettings.renderScale, 0.25f, 1.0f);
    

    // Positional arguments (non-flag/option) are treated as file paths passed to the Editor
    // argh's pos_args() index 0 is the program name; file paths start from index 1
    Vector<const char*> files(GLOBAL_ALLOC);
    for (size_t i = 1; i < cmdl.pos_args().size(); ++i)
        files.push_back(cmdl.pos_args()[i].c_str());
    GContext->files = Span<const char*>(files.data(), files.data() + files.size());

    while (!mainLoop()) {}
    LOG(SDLMain, LogInfo, "Quitting...");
    ClearMaterialTexturePreviewCache();
    EditorCleanup();
    ImGui_ImplFoundation_Shutdown();
    DestroyContext();
}
