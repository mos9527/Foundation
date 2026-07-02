#pragma once
#include <Core/Paths.hpp>
#include <RHIVulkan/Application.hpp>
#include <RenderCore/Renderer.hpp>
#include <Renderer/GPUScene.hpp>
#include <Renderer/Renderer.hpp>
#include <Renderer/Postprocess.hpp>
#include <Renderer/Texture.hpp>
#include <Math/Math.hpp>
#include <Math/ModelViewProjection.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <RenderUtils/PSFullscreen.hpp>
#include <SDL3/SDL_main.h>
#include <array>
#include <tuple>
#include <vector>

void Examples_ReportFatalException();

#if defined(__ANDROID__) && !defined(FOUNDATION_EXAMPLES_IMPLEMENTATION)
#ifdef main
#undef main
#endif
int Foundation_ExampleMain(int argc, char** argv);
extern "C" int SDL_main(int argc, char** argv)
{
    try
    {
        return Foundation_ExampleMain(argc, argv);
    }
    catch (...)
    {
        Examples_ReportFatalException();
        return 1;
    }
}
#define main Foundation_ExampleMain
#endif

using namespace Foundation;
using namespace Core;
using namespace Math;
using namespace RenderCore;

// [renderer, app, device, swapchain]
constexpr int Examples_SDLWindowFlagsVulkan = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_VULKAN;
// Common command-line options shared by all examples (mirrors Editor::SDLMain):
//   -h, --help        Show usage and exit
//   -g, --gpu <id>    Select GPU device index
//   -l, --list-gpus   List available GPU devices and exit
//
using ExampleVulkanContext =
    std::tuple<Renderer*, VulkanApplication*, RHIApplicationScopedHandle<RHIDevice>, RHIDeviceScopedHandle<RHISwapchain>>;

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
    static constexpr size_t kMaxHudLines = 32;

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
    // Immediate-mode HUD buffer: Examples_Text/Button/Slider claim lines from here in order.
    // Examples_BeginControls clears it and resets hudCount for the new frame.
    std::array<RenderUtils::CSDebugTextData, kMaxHudLines> hud{};
    size_t hudCount{};
    std::array<SDL_Keycode, kMaxPressedKeys> pressedKeys{};
    std::array<ExampleTouchState, 4> touches{};
    std::vector<ExampleClickableRegion> clickableRegions;
    std::vector<ExampleClickableRegion> nextClickableRegions;

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

enum class ExampleGPUSceneRenderMode
{
    Raster,
    PathTracer
};

struct ExampleGPUSceneRenderState
{
    ExampleGPUSceneRenderMode mode = ExampleGPUSceneRenderMode::PathTracer;
    bool renderPaused = false;
    float renderScale = 0.10f;
    RendererConfig config{};
    RendererOutputs outputs{};
    RHIExtent2D renderExtent{};
};

// Pass desc.present = false (with desc.renderExtent set) to initialize headlessly:
// no SDL window or Vulkan WSI is required, no swapchain is created, and the returned
// swapchain handle is invalid. This works on software backends (e.g. Lavapipe) without
// surface extensions. In that case `window` is ignored and may be nullptr.
ExampleVulkanContext Examples_InitVulkan(SDL_Window* window, int argc, char** argv, RendererDesc desc = {});

// Drains all pending SDL events into the frame input state, resizing the swapchain as needed.
bool Examples_PollEvents(SDL_Window* window, Renderer* renderer, RHIDeviceScopedHandle<RHISwapchain>& swap,
                         ExampleInputState& input, SDL_Event* outLastEvent = nullptr);

// Compatibility wrapper for simple examples that only need close/resize and optionally the last event.
bool Examples_ShouldClose(SDL_Window* window, Renderer* renderer, RHIDeviceScopedHandle<RHISwapchain>& swap,
                          SDL_Event* outEvent = nullptr);

void Examples_BeginFrameInput(ExampleInputState& input);
// Resets the layout cursor to (x, y) and clears ExampleInputState::hud for the frame; every
// Examples_Text/Button/Slider call below this claims the next line automatically.
void Examples_BeginControls(ExampleInputState& input, int x = 16, int y = 16);
void Examples_SameLine(ExampleInputState& input, int spacing = 16);
void Examples_Text(ExampleInputState& input, StringView text);
bool Examples_Button(ExampleInputState& input, StringView text);
// Text stepper: "Label [-][====------][+] value". [-]/[+] step by `step`; the bar itself is also a
// clickable/draggable control that jumps/scrubs the value to the pointer's position along it.
bool Examples_Slider(ExampleInputState& input, StringView label, float& value, float minValue, float maxValue,
                     float step = 0.01f, const char* unit = "x", bool draggable = true);
// The HUD lines claimed so far this frame; pass to createCSDebugTextPassBackBuffer (or
// Examples_GPUSceneBuildRenderGraph does this internally).
Span<const RenderUtils::CSDebugTextData> Examples_HudLines(ExampleInputState const& input);
const char* Examples_GPUSceneModeName(ExampleGPUSceneRenderMode mode);
void Examples_GPUSceneToggleMode(ExampleGPUSceneRenderState& state);
void Examples_GPUSceneBuildRenderGraph(Renderer* renderer, RendererUBO* ubo, GPUScene* gpu,
                                       ExampleGPUSceneRenderState& state, ExampleInputState const& input,
                                       RHIExtent2D swapchainExtent);
void Examples_GPUSceneFillCameraUBO(RendererUBO& ubo, Renderer* renderer, FExampleOrbitCamera const& camera,
                                    RendererConfig const& config);
void Examples_NewFrame(Renderer* renderer);
bool Examples_NewFrame(SDL_Window* window, Renderer* renderer, RHIDeviceScopedHandle<RHISwapchain>& swapchain);
void Examples_DestroyVulkan(SDL_Window* window, Renderer* renderer, VulkanApplication* app,
                            RHIApplicationScopedHandle<RHIDevice>& device,
                            RHIDeviceScopedHandle<RHISwapchain>& swapchain);

// Writes an 8-bit-per-channel image (e.g. from a readback buffer) to a PNG at `path`,
// then opens it with the OS default viewer via SDL_OpenURL. `strideBytes` defaults to
// width * channels when zero. Link ThirdParty_STB in targets that call this.
//   channels: 1 (grey), 2 (grey+alpha), 3 (RGB), 4 (RGBA)
void Examples_DumpAndOpenImage(StringView path, RHIExtent2D extent, void const* data,
                               int channels = 4, int strideBytes = 0);

// Simplified example tonemap / postprocess pass.
//   Raster: samples `outputs.diffuse` as-is.
//   PT:     samples `outputs.diffuse + outputs.specular`.
//   Output: clamped to [0,1] then gamma-corrected (1/2.2) into an explicit LDR
//           PostprocessBuffer RTV (R8G8B8A8Unorm). When the renderer presents, that
//           RTV is blitted to the backbuffer so windowed examples display it; the RTV
//           handle is returned so headless examples can read it back. No view LUTs,
//           exposure, or debug-AOV branches - this is the minimal display path.
ResourceHandle Examples_InsertBasicTonemapPasses(Renderer* renderer, RendererOutputs const& outputs,
                                                 bool isPathTracer);

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
