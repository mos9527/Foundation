#pragma once
#include <Core/BuildInfo.hpp>
#include <Core/JobSystem.hpp>
#include <Core/Container.hpp>
#include <RHIVulkan/Application.hpp>
#include <RenderCore/Renderer.hpp>
#include <RenderCore/Presenter.hpp>
#include <RHICore/Surface.hpp>
#include <Renderer/GPUScene.hpp>
#include <Renderer/Renderer.hpp>
#include <Renderer/Texture.hpp>
#include <Renderer/Mesh.hpp>
#include <Math/Math.hpp>
#include <Math/ModelViewProjection.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <RenderUtils/PSFullscreen.hpp>
#include <SDL3/SDL_main.h>
void Examples_ReportFatalException();

#if defined(__ANDROID__) && !defined(FOUNDATION_EXAMPLES_IMPLEMENTATION)
#ifdef main
#undef main
#endif
int Foundation_ExampleMain(int argc, char** argv);
extern "C" int SDL_main(int argc, char** argv)
{
    return Foundation_ExampleMain(argc, argv);
}
#define main Foundation_ExampleMain
#endif

using namespace Foundation;
using namespace Core;
using namespace Math;
using namespace RenderCore;

// ExampleVulkanContext members, in structured-binding order: [renderer, app, device, surface, swapchain, presenter]
constexpr int Examples_SDLWindowFlagsVulkan = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
// Common command-line options shared by all examples (mirrors Editor::SDLMain):
//   -h, --help        Show usage and exit
//   -g, --gpu <id>    Select GPU device index
//   -l, --list-gpus   List available GPU devices and exit
//
struct ExampleVulkanContext
{
    UniquePtr<VulkanApplication> app;
    RHIApplicationScopedHandle<RHIDevice> device;
    UniquePtr<JobSystem> jobs;
    RHIDeviceScopedHandle<RHIPipelineStateCache> psoCache;
    RHIDeviceScopedHandle<RHISurface> surface;
    RHIDeviceScopedHandle<RHISwapchain> swapchain;
    UniquePtr<Presenter> presenter;
    UniquePtr<Renderer> renderer;
};

struct ExampleClickableRegion
{
    uint32_t id{};
    int x{};
    int y{};
    int w{};
    int h{};
};

struct ExampleTouchState
{
    SDL_FingerID fingerId{};
    float2 position{};
    float2 previous{};
    bool active{};
    bool capturedByHud{};
};

struct ExampleInputState
{
    static constexpr size_t kMaxPressedKeys = 16;
    static constexpr size_t kMaxHudLines = 64;
    static constexpr size_t kMaxClickableRegions = kMaxHudLines;
    // Set to true whenever the window is resized. Clear manually.
    // You can also set this to true to force a resize for e.g. Renderer to be rebuilt.
    bool wantResizeOrRebuild{true};
    // Inputs
    bool keyForward{};
    bool keyBack{};
    bool keyLeft{};
    bool keyRight{};
    bool keyFast{};
    bool mouseCapturedByHud{};
    bool pointerDown{};
    float2 orbitDelta{};
    float2 panDelta{};
    float2 move{};
    float zoomDelta{};
    SDL_Event lastEvent{};
    uint32_t clickedControl{};
    uint32_t activeControl{};
    uint32_t nextControlId = 1u;
    uint32_t pressedKeyCount{};
    float2 pointerPosition{};
    float2 clickPosition{};
    // UI states
    static constexpr size_t kMaxUiStyleStack = 8;
    int uiX = 16;
    int uiY = 16;
    int uiStartX = 16;
    int uiLineHeight = 24;
    int uiYSpacing = 2;
    int uiSameLineSpacing = 16;
    int uiLastItemWidth{};
    int uiLastItemHeight{};
    int uiLastItemX{};
    int uiLastItemY{};
    bool uiSameLine{};
    int uiScale = 2;
    int uiColor = -1;
    size_t uiScaleStackCount{};
    size_t uiColorStackCount{};
    Array<int, kMaxUiStyleStack> uiScaleStack{};
    Array<int, kMaxUiStyleStack> uiColorStack{};
    Array<RenderUtils::CSDebugTextData, kMaxHudLines> hud{};
    size_t hudCount{};
    Array<SDL_Keycode, kMaxPressedKeys> pressedKeys{};
    Array<ExampleTouchState, 4> touches{};
    Vector<ExampleClickableRegion> clickableRegions{GLOBAL_ALLOC};
    Vector<ExampleClickableRegion> nextClickableRegions{GLOBAL_ALLOC};

    ExampleInputState()
    {
        clickableRegions.reserve(kMaxClickableRegions);
        nextClickableRegions.reserve(kMaxClickableRegions);
    }

    bool KeyPressed(SDL_Keycode key) const
    {
        for (uint32_t i = 0; i < pressedKeyCount; ++i)
            if (pressedKeys[i] == key)
                return true;
        return false;
    }
};

struct FExampleOrbitCamera
{
    static constexpr char kControlsText[] =
        "Drag orbit | 2-finger/right-drag pan | pinch/wheel zoom | WASD move";

    float3 center{};
    float3 position{};
    float radius = 1.0f;
    quat rot = quat(1.0f, 0.0f, 0.0f, 0.0f);
    float zNear = 0.01f;
    float fovY = radians(45.0f);
    float aspect = 1.0f;
    mat4 view{};
    mat4 proj{};
    float moveSpeed = 2.0f;

    bool Update(ExampleInputState const& input, float dt);
    void RefreshMatrices();
};

void Examples_UpdateCameraUBO(RendererUBO& ubo, Renderer* renderer, FExampleOrbitCamera& camera,
                              RendererConfig const& config);

ExampleVulkanContext Examples_InitVulkan(SDL_Window* window, int argc, char** argv, RendererDesc desc = {});
bool Examples_PollEvents(SDL_Window* window, ExampleVulkanContext& ctx, ExampleInputState& input, SDL_Event* outLastEvent = nullptr,
                         void (*processEvent)(SDL_Event*) = nullptr);
bool Examples_ShouldClose(SDL_Window* window, ExampleVulkanContext& ctx, SDL_Event* outEvent = nullptr);

void Examples_BeginFrameInput(ExampleInputState& input);
void Examples_BeginControls(ExampleInputState& input, int x = 16, int y = 16);
void Examples_SameLine(ExampleInputState& input, int spacing = 16);
void Examples_PushScale(ExampleInputState& input, int scale);
void Examples_PopScale(ExampleInputState& input);
void Examples_PushColor(ExampleInputState& input, int col32);
void Examples_PushColor(ExampleInputState& input, int r, int g, int b, int a = 255);
void Examples_PopColor(ExampleInputState& input);
void Examples_Text(ExampleInputState& input, StringView text);
bool Examples_Button(ExampleInputState& input, StringView text);
bool Examples_Slider(ExampleInputState& input, StringView label, float& value, float minValue, float maxValue,
                     float step = 0.01f, const char* unit = "x", bool draggable = true);
Span<const RenderUtils::CSDebugTextData> Examples_HudLines(ExampleInputState const& input);
void Examples_NewFrame(Renderer* renderer); // Headless
void Examples_NewFrame(SDL_Window* window, ExampleVulkanContext& ctx); // To Window WSI
void Examples_ResetRenderer(ExampleVulkanContext& ctx, RendererDesc desc = {});
void Examples_DestroyVulkan(SDL_Window* window, ExampleVulkanContext& ctx);
bool Examples_CreateSwapchain(SDL_Window* window, RHIDevice* device, RHIDeviceScopedHandle<RHISurface>& outSurface, RHIDeviceScopedHandle<RHISwapchain>& outSwapchain);
void Examples_DumpAndOpenImage(RHIApplication const& app, StringView path, RHIExtent2D extent, void const* data,
                               int channels = 4, int strideBytes = 0);
float Examples_GetTime();


struct ExampleFpsCounter
{
    size_t lastTick{};
    size_t frames{};
    float fps{};
    float Update()
    {
        size_t now = SDL_GetTicksNS();
        size_t delta = now - lastTick;
        if (delta >= 1e9)
        {
            fps = 1e9 * (static_cast<float>(frames) / delta);
            frames = 0;
            lastTick = now;
        }
        frames++;
        return fps;
    }
};
