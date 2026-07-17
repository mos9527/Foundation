#define SDL_MAIN_HANDLED
#define FOUNDATION_EXAMPLES_IMPLEMENTATION
#include "Examples.hpp"

#include <Renderer/RasterEffects.hpp>
#include <RenderCore/Presenter.hpp>
#include <argh.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stb_image_write.h>

using namespace Foundation;
using namespace Core;
using namespace Math;
using namespace RenderCore;

namespace
{
constexpr int kExampleUiCharWidth = 9;
constexpr int kExampleUiCharHeight = 16;
constexpr int kExampleUiDefaultColor = -1;
constexpr int kExampleUiActiveColor = 0xff00ff00;
const RasterGTAOConfig kExampleRasterGTAOConfig{};
const RasterMotionBlurConfig kExampleRasterMotionBlurConfig{};
const RasterEffect kExampleRasterEffects[] = {
    MakeRasterGTAOEffect(&kExampleRasterGTAOConfig),
    MakeRasterMotionBlurEffect(&kExampleRasterMotionBlurConfig),
};

constexpr RHISurfaceFormat kFormatPreferenceList[] = {
    {RHIResourceFormat::R8G8B8A8Unorm, RHIColorSpace::SrgbNonLinear},
    {RHIResourceFormat::B8G8R8A8Unrom, RHIColorSpace::SrgbNonLinear},
};

constexpr RHISwapchainPresentMode kPresentModePreferenceList[] = {
    RHISwapchainPresentMode::Mailbox, RHISwapchainPresentMode::Tearing, RHISwapchainPresentMode::Fifo};

String PipelineCachePathForDevice(RHIDevice const& device)
{
    auto key = device.GetPipelineCacheKey();
    return PathsResolve(fmt::format("Cache/PipelineCache/Vulkan/pso-cache-{:016x}-{:016x}.bin", key.high, key.low));
}

Vector<unsigned char> LoadPipelineCacheBytes(StringView path, Allocator* allocator)
{
    Vector<unsigned char> data(allocator);
    if (!std::filesystem::exists(path.data()))
        return data;

    std::ifstream file(path.data(), std::ios::binary | std::ios::ate);
    if (!file)
    {
        LOG(Examples, LogWarn, "Failed to open pipeline cache file for reading: {}", path);
        return data;
    }

    auto fileSize = file.tellg();
    if (fileSize <= std::streampos(0))
        return data;

    auto size = static_cast<size_t>(fileSize);
    data.resize(size);
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
    {
        LOG(Examples, LogWarn, "Failed to read pipeline cache file: {}", path);
        data.clear();
    }
    return data;
}

void ClearStalePipelineCaches(StringView activePath)
{
    std::filesystem::path cacheDir = std::filesystem::path(activePath).parent_path();
    if (!std::filesystem::exists(cacheDir))
        return;

    std::filesystem::path active(activePath);
    for (auto const& entry : std::filesystem::directory_iterator(cacheDir))
    {
        if (!entry.is_regular_file() || entry.path() == active)
            continue;

        auto filename = entry.path().filename().string();
        if (!filename.starts_with("pso-cache-") || !filename.ends_with(".bin"))
            continue;

        LOG(Examples, LogInfo, "Clearing stale pipeline cache: {}", entry.path().string());
        std::error_code ec;
        std::filesystem::remove(entry.path(), ec);
    }
}

void SavePipelineCache(RHIPipelineStateCache const& cache, StringView path, Allocator* allocator)
{
    size_t size = cache.GetSerializedDataSize();
    if (size == 0)
        return;

    Vector<unsigned char> data(size, allocator);
    size_t written = cache.WriteSerializedData(data);
    if (written == 0)
        return;

    data.resize(written);
    std::filesystem::path fsPath(path);
    std::filesystem::create_directories(fsPath.parent_path());
    std::ofstream file(fsPath, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        LOG(Examples, LogWarn, "Failed to open pipeline cache file for writing: {}", path);
        return;
    }

    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file)
    {
        LOG(Examples, LogWarn, "Failed to write pipeline cache file: {}", path);
    }
    else
    {
        LOG(Examples, LogInfo, "Saved pipeline cache: {} ({} bytes)", path, data.size());
    }
}

struct PipelineCacheContext
{
    Renderer* renderer{};
    RHIDevice* device{};
    RHIDeviceScopedHandle<RHIPipelineStateCache> cache;
    String path;
};

std::vector<PipelineCacheContext>& PipelineCacheContexts()
{
    static std::vector<PipelineCacheContext> contexts;
    return contexts;
}

bool CreateSwapchain(SDL_Window* window, RHIDevice* device, RHIDeviceScopedHandle<RHISwapchain>& outSwap)
{
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0)
        return false;
    LOG(RenderApplication, LogDebug, "Creating swapchain ({}x{})", w, h);
    device->WaitIdle();
    if (outSwap)
        outSwap.Reset();
    auto format = Ranges::FirstOf(Views::all(kFormatPreferenceList) |
                                  Views::filter(Ranges::ContainedBy(device->GetSwapchainSupportedFormats())));
    auto present = Ranges::FirstOf(Views::all(kPresentModePreferenceList) |
                                   Views::filter(Ranges::ContainedBy(device->GetSwapchainSupportedPresentModes())));
    CHECK_MSG(format.has_value(), "No supported swapchain format found!");
    LOG(RenderApplication, LogDebug, "Selected swapchain format: {} with color space: {}", format.value().format,
        format.value().colorSpace);
    CHECK_MSG(present.has_value(), "No supported presentation mode found!");
    LOG(RenderApplication, LogDebug, "Selected swapchain present mode: {}", present.value());
    outSwap = device->CreateSwapchain(RHISwapchain::SwapchainDesc{
        .format = format.value().format,
        .colorSpace = format.value().colorSpace,
        .extents = RHIExtent3D{w, h, 1},
        .minBufferCount = 3,
        .presentMode = present.value(),
    });
    return true;
}

#if defined(__ANDROID__)
// Bridges Foundation::Core::PathsResolve to SDL's APK-asset loader. Registered
// in Examples_InitVulkan so shaders/bundled assets are lazily materialized out
// of the APK on first PathsResolve access (see Source/Core/Paths.cpp).
void* Examples_SDLAssetLoader(const char* relPath, size_t* outSize)
{
    return SDL_LoadFile(relPath, outSize);
}
#endif

// Surface uncaught exceptions instead of dying with a bare SIGABRT. The
// renderer/PSO layer throws std::runtime_error on failure (missing shader,
// unsupported feature, etc.); this turns that into a visible prompt plus a
// logcat line on Android (and a message box elsewhere).
void Examples_TerminateHandler()
{
    Examples_ReportFatalException();
    std::abort();
}

bool EventBelongsToWindow(SDL_Event const& event, SDL_WindowID windowID)
{
    switch (event.type)
    {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return event.key.windowID == windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.windowID == windowID;
    case SDL_EVENT_MOUSE_MOTION:
        return event.motion.windowID == windowID;
    case SDL_EVENT_MOUSE_WHEEL:
        return event.wheel.windowID == windowID;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        return event.window.windowID == windowID;
    default:
        return true;
    }
}

bool HitTest(ExampleInputState const& input, float x, float y, uint32_t& outControlId)
{
    for (ExampleClickableRegion const& region : input.clickableRegions)
    {
        if (x >= static_cast<float>(region.x) && y >= static_cast<float>(region.y) &&
            x < static_cast<float>(region.x + region.w) && y < static_cast<float>(region.y + region.h))
        {
            outControlId = region.id;
            return true;
        }
    }
    return false;
}

int DebugTextWidth(RenderUtils::CSDebugTextData const& line)
{
    return std::max(1, static_cast<int>(std::strlen(line.szText)) * kExampleUiCharWidth * line.scale);
}

int DebugTextHeight(RenderUtils::CSDebugTextData const& line)
{
    return std::max(1, kExampleUiCharHeight * line.scale);
}

void PlaceControl(ExampleInputState& input, RenderUtils::CSDebugTextData& line)
{
    if (input.uiSameLine)
    {
        input.uiX = input.uiLastItemX + input.uiLastItemWidth + input.uiSameLineSpacing;
        input.uiY = input.uiLastItemY;
        input.uiSameLine = false;
    }
    line.x = input.uiX;
    line.y = input.uiY;
}

void AdvanceControl(ExampleInputState& input, RenderUtils::CSDebugTextData const& line)
{
    const int width = DebugTextWidth(line);
    const int height = DebugTextHeight(line);
    input.uiLastItemX = line.x;
    input.uiLastItemY = line.y;
    input.uiLastItemWidth = width;
    input.uiLastItemHeight = height;
    input.uiLineHeight = height + input.uiYSpacing;
    input.uiY = line.y + input.uiLineHeight;
    input.uiX = input.uiStartX;
}

void RegisterControl(ExampleInputState& input, uint32_t id, RenderUtils::CSDebugTextData const& line)
{
    input.nextClickableRegions.push_back({
        .id = id,
        .x = line.x,
        .y = line.y,
        .w = DebugTextWidth(line),
        .h = DebugTextHeight(line),
    });
}

float PointerFractionForLine(ExampleInputState const& input, RenderUtils::CSDebugTextData const& line)
{
    const float width = static_cast<float>(std::max(1, DebugTextWidth(line)));
    return std::clamp((input.pointerPosition.x - static_cast<float>(line.x)) / width, 0.0f, 1.0f);
}

RenderUtils::CSDebugTextData& NextHudLine(ExampleInputState& input)
{
    CHECK_MSG(input.hudCount < input.hud.size(), "Exceeded ExampleInputState::kMaxHudLines");
    return input.hud[input.hudCount++];
}

float2 TouchPositionPixels(SDL_Window* window, SDL_TouchFingerEvent const& event)
{
    int w = 1;
    int h = 1;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return float2(event.x * static_cast<float>(w), event.y * static_cast<float>(h));
}

ExampleTouchState* FindTouch(ExampleInputState& input, SDL_FingerID fingerId)
{
    for (ExampleTouchState& touch : input.touches)
        if (touch.active && touch.fingerId == fingerId)
            return &touch;
    return nullptr;
}

ExampleTouchState* AllocateTouch(ExampleInputState& input, SDL_FingerID fingerId)
{
    if (ExampleTouchState* touch = FindTouch(input, fingerId))
        return touch;
    for (ExampleTouchState& touch : input.touches)
    {
        if (!touch.active)
        {
            touch = {};
            touch.active = true;
            touch.fingerId = fingerId;
            return &touch;
        }
    }
    return nullptr;
}

bool IsTouchMouseEvent(SDL_MouseID mouseId)
{
#if defined(SDL_TOUCH_MOUSEID)
    return mouseId == SDL_TOUCH_MOUSEID;
#else
    (void)mouseId;
    return false;
#endif
}

bool HasActiveTouches(ExampleInputState const& input)
{
    for (ExampleTouchState const& touch : input.touches)
        if (touch.active)
            return true;
    return false;
}

void RecordKeyPress(SDL_Keycode key, ExampleInputState& input)
{
    if (input.pressedKeyCount >= ExampleInputState::kMaxPressedKeys)
        return;
    input.pressedKeys[input.pressedKeyCount++] = key;
}

void UpdateMovementKey(SDL_Keycode key, bool pressed, ExampleInputState& input)
{
    switch (key)
    {
    case SDLK_W:
        input.keyForward = pressed;
        break;
    case SDLK_A:
        input.keyLeft = pressed;
        break;
    case SDLK_S:
        input.keyBack = pressed;
        break;
    case SDLK_D:
        input.keyRight = pressed;
        break;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        input.keyFast = pressed;
        break;
    default:
        break;
    }
}

void UpdateMoveVector(ExampleInputState& input)
{
    input.move = float2((input.keyRight ? 1.0f : 0.0f) - (input.keyLeft ? 1.0f : 0.0f),
                        (input.keyForward ? 1.0f : 0.0f) - (input.keyBack ? 1.0f : 0.0f));
}

void ProcessTouchMotion(ExampleInputState& input, ExampleTouchState& movedTouch)
{
    if (movedTouch.capturedByHud)
        return;

    int activeCameraTouches = 0;
    ExampleTouchState* first = nullptr;
    ExampleTouchState* second = nullptr;
    for (ExampleTouchState& touch : input.touches)
    {
        if (!touch.active || touch.capturedByHud)
            continue;
        if (!first)
            first = &touch;
        else if (!second)
            second = &touch;
        ++activeCameraTouches;
    }

    const float2 delta = movedTouch.position - movedTouch.previous;
    if (activeCameraTouches <= 1)
    {
        input.orbitDelta += delta;
        return;
    }

    if (first && second)
    {
        const float2 oldFirst = first == &movedTouch ? first->previous : first->position;
        const float2 oldSecond = second == &movedTouch ? second->previous : second->position;
        const float2 oldCenter = (oldFirst + oldSecond) * 0.5f;
        const float2 newCenter = (first->position + second->position) * 0.5f;
        input.panDelta += newCenter - oldCenter;

        float oldDistance = length(oldFirst - oldSecond);
        float newDistance = length(first->position - second->position);
        if (oldDistance > 1e-3f)
            input.zoomDelta += (newDistance - oldDistance) / 120.0f;
    }
}

void ProcessEvent(SDL_Window* window, SDL_Event const& event, ExampleInputState& input,
                  Renderer* renderer, RHIDeviceScopedHandle<RHISurface>& surface, RHIDeviceScopedHandle<RHISwapchain>& swap)
{
    switch (event.type)
    {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        try
        {
            if (Examples_CreateSwapchain(window, swap.mFactory, surface, swap))
                renderer->SetSwapchain(swap);
        }
        catch (std::exception const& e)
        {
            LOG(Examples, LogWarn, "Swapchain event recreation failed: {}", e.what());
            try
            {
                RHIDevice* device = swap.mFactory;
            swap.Reset();
            surface.Reset();
            if (Examples_CreateSwapchain(window, device, surface, swap))
                    renderer->SetSwapchain(swap);
            }
            catch (std::exception const& refreshError)
            {
                LOG(Examples, LogWarn, "Swapchain event recreation deferred: {}", refreshError.what());
            }
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (!event.key.repeat)
            RecordKeyPress(event.key.key, input);
        UpdateMovementKey(event.key.key, true, input);
        break;
    case SDL_EVENT_KEY_UP:
        UpdateMovementKey(event.key.key, false, input);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (IsTouchMouseEvent(event.button.which))
            break;
        if (event.button.button == SDL_BUTTON_LEFT)
        {
            input.pointerDown = true;
            input.pointerPosition = float2(event.button.x, event.button.y);
            input.clickPosition = input.pointerPosition;
            uint32_t controlId = 0u;
            if (HitTest(input, event.button.x, event.button.y, controlId))
            {
                input.clickedControl = controlId;
                input.activeControl = controlId;
                input.mouseCapturedByHud = true;
            }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (IsTouchMouseEvent(event.button.which))
            break;
        if (event.button.button == SDL_BUTTON_LEFT)
        {
            input.pointerDown = false;
            input.mouseCapturedByHud = false;
            input.activeControl = 0u;
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (IsTouchMouseEvent(event.motion.which))
            break;
        input.pointerPosition = float2(event.motion.x, event.motion.y);
        if (!input.mouseCapturedByHud)
        {
            if (event.motion.state & SDL_BUTTON_LMASK)
                input.orbitDelta += float2(event.motion.xrel, event.motion.yrel);
            if (event.motion.state & SDL_BUTTON_RMASK)
                input.panDelta += float2(event.motion.xrel, event.motion.yrel);
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (IsTouchMouseEvent(event.wheel.which))
            break;
        input.zoomDelta += event.wheel.y;
        break;
    case SDL_EVENT_FINGER_DOWN:
    {
        ExampleTouchState* touch = AllocateTouch(input, event.tfinger.fingerID);
        if (!touch)
            break;
        touch->position = TouchPositionPixels(window, event.tfinger);
        touch->previous = touch->position;
        input.pointerDown = true;
        input.pointerPosition = touch->position;
        input.clickPosition = touch->position;
        uint32_t controlId = 0u;
        if (HitTest(input, touch->position.x, touch->position.y, controlId))
        {
            input.clickedControl = controlId;
            input.activeControl = controlId;
            touch->capturedByHud = true;
        }
        break;
    }
    case SDL_EVENT_FINGER_MOTION:
    {
        ExampleTouchState* touch = FindTouch(input, event.tfinger.fingerID);
        if (!touch)
            break;
        touch->previous = touch->position;
        touch->position = TouchPositionPixels(window, event.tfinger);
        input.pointerPosition = touch->position;
        ProcessTouchMotion(input, *touch);
        break;
    }
    case SDL_EVENT_FINGER_UP:
    {
        if (ExampleTouchState* touch = FindTouch(input, event.tfinger.fingerID))
        {
            if (touch->capturedByHud)
                input.activeControl = 0u;
            *touch = {};
        }
        input.pointerDown = HasActiveTouches(input);
        break;
    }
    default:
        break;
    }
}
} // namespace

bool Examples_CreateSwapchain(SDL_Window* window, RHIDevice* device, RHIDeviceScopedHandle<RHISurface>& outSurface, RHIDeviceScopedHandle<RHISwapchain>& outSwap)
{
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (w <= 0 || h <= 0)
        return false;
    LOG(RenderApplication, LogDebug, "Creating swapchain ({}x{})", w, h);
    device->WaitIdle();
    if (outSwap)
        outSwap.Reset();
    if (!outSurface)
        outSurface = device->CreateSurface(RHISurface::SurfaceDesc{.windowHandle = window});

    auto format = Ranges::FirstOf(Views::all(kFormatPreferenceList) |
                                  Views::filter(Ranges::ContainedBy(outSurface->GetSupportedFormats())));
    auto present = Ranges::FirstOf(Views::all(kPresentModePreferenceList) |
                                   Views::filter(Ranges::ContainedBy(outSurface->GetSupportedPresentModes())));
    CHECK_MSG(format.has_value(), "No supported swapchain format found!");
    LOG(RenderApplication, LogDebug, "Selected swapchain format: {} with color space: {}", format.value().format,
        format.value().colorSpace);
    CHECK_MSG(present.has_value(), "No supported presentation mode found!");
    LOG(RenderApplication, LogDebug, "Selected swapchain present mode: {}", present.value());
    outSwap = device->CreateSwapchain(RHISwapchain::SwapchainDesc{
        .format = format.value().format,
        .colorSpace = format.value().colorSpace,
        .extents = RHIExtent3D{w, h, 1},
        .minBufferCount = 3,
        .presentMode = present.value(),
        .surface = outSurface,
    });
    return true;
}

void Examples_ReportFatalException()
{
    try
    {
        if (auto ep = std::current_exception())
            std::rethrow_exception(ep);
    }
    catch (std::exception const& e)
    {
        LOG(Examples, LogError, "Unhandled exception: {}", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Foundation Example", e.what(), nullptr);
    }
    catch (...)
    {
        LOG(Examples, LogError, "Unhandled exception: unknown");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Foundation Example", "Unhandled exception", nullptr);
    }
}

ExampleVulkanContext Examples_InitVulkan(SDL_Window* window, int argc, char** argv, RendererDesc desc)
{
    argh::parser cmdl(argc, argv, argh::parser::PREFER_PARAM_FOR_UNREG_OPTION);
    if (cmdl[{"-h", "--help"}])
    {
        fmt::println("Usage: {} [options]", argv[0]);
        fmt::println("Options:");
        fmt::println("\t-h, --help\t\tShow this help message");
        fmt::println("\t-g, --gpu <id>\t\tSpecify GPU device index");
        fmt::println("\t-l, --list-gpus\t\tList available GPU devices");
        std::exit(0);
    }
    const bool headless = window == nullptr;
    if (cmdl[{"-l", "--list-gpus"}])
    {
        auto* app = Construct<VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC, headless);
        for (auto const& d : app->EnumerateDevices())
            fmt::println("[{}] {}", d.id, d.name);
        Destruct(GLOBAL_ALLOC, app);
        std::exit(0);
    }
    int gpuId = 0;
    cmdl({"-g", "--gpu"}, 0) >> gpuId;
    if (headless)
        PathsInit(argv[0]);
    else
    {
        std::set_terminate(Examples_TerminateHandler);
#if defined(__ANDROID__)
        const char* prefPath = SDL_GetPrefPath("foundation", "examples");
        PathsRegisterAssetLoader(&Examples_SDLAssetLoader);
        PathsInitFromDir(prefPath);
        SDL_free((void*)prefPath);
#else
        PathsInitFromDir(SDL_GetBasePath());
#endif
    }
    auto app = Construct<VulkanApplication>(GLOBAL_ALLOC, GLOBAL_ALLOC, headless);
    auto device = app->CreateDevice({.id = static_cast<uint32_t>(gpuId)});
    RHIDeviceScopedHandle<RHISwapchain> swap;
    RHIDeviceScopedHandle<RHISurface> surface;
    if (!headless)
        Examples_CreateSwapchain(window, device.Get(), surface, swap);
    RHIDeviceScopedHandle<RHIPipelineStateCache> psoCache;
    String psoCachePath;
    if (!desc.pipelineCache)
    {
        psoCachePath = PipelineCachePathForDevice(*device.Get());
        ClearStalePipelineCaches(psoCachePath);
        auto psoCacheBytes = LoadPipelineCacheBytes(psoCachePath, GLOBAL_ALLOC);
        psoCache = device->CreatePipelineCache({
            .initialData = Span<const unsigned char>(psoCacheBytes.data(), psoCacheBytes.size())
        });
        LOG(Examples, LogInfo, "Pipeline cache {}: {} ({} bytes)", psoCache->GetImportStatus(), psoCachePath,
            psoCacheBytes.size());
        desc.pipelineCache = psoCache.Get();
    }
    auto renderer = Construct<Renderer>(GLOBAL_ALLOC, desc, device, swap, GLOBAL_ALLOC);
    if (psoCache)
    {
        PipelineCacheContexts().push_back({
            .renderer = renderer,
            .device = device.Get(),
            .cache = std::move(psoCache),
            .path = std::move(psoCachePath),
        });
    }
    Presenter* presenter = nullptr;
    if (swap.IsValid())
        presenter = Construct<Presenter>(GLOBAL_ALLOC, device.Get(), swap, GLOBAL_ALLOC);
    return std::make_tuple(renderer, app, std::move(device), std::move(surface), std::move(swap), presenter);
}

bool Examples_PollEvents(SDL_Window* window, Renderer* renderer, RHIDeviceScopedHandle<RHISurface>& surface, RHIDeviceScopedHandle<RHISwapchain>& swap,
                         ExampleInputState& input, SDL_Event* outLastEvent,
                         void (*processEvent)(SDL_Event*))
{
    bool shouldClose = false;
    SDL_Event event{};
    SDL_WindowID windowID = window ? SDL_GetWindowID(window) : 0;
    while (SDL_PollEvent(&event))
    {
        if (outLastEvent)
            *outLastEvent = event;
        input.lastEvent = event;
        if (event.type == SDL_EVENT_QUIT)
        {
            shouldClose = true;
            continue;
        }
        if (window && !EventBelongsToWindow(event, windowID))
            continue;
        if (processEvent)
            processEvent(&event);
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        {
            shouldClose = true;
            continue;
        }
        ProcessEvent(window, event, input, renderer, surface, swap);
    }
    UpdateMoveVector(input);
    return shouldClose;
}

bool Examples_ShouldClose(SDL_Window* window, Renderer* renderer, RHIDeviceScopedHandle<RHISurface>& surface, RHIDeviceScopedHandle<RHISwapchain>& swap,
                          SDL_Event* outEvent)
{
    ExampleInputState input{};
    SDL_Event event{};
    if (!SDL_PollEvent(&event))
    {
        if (outEvent)
            *outEvent = event;
        return false;
    }
    if (outEvent)
        *outEvent = event;
    if (event.type == SDL_EVENT_QUIT)
        return true;
    if (window && !EventBelongsToWindow(event, SDL_GetWindowID(window)))
        return false;
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
        return true;
    ProcessEvent(window, event, input, renderer, surface, swap);
    return false;
}

void Examples_BeginFrameInput(ExampleInputState& input)
{
    input.orbitDelta = float2(0.0f);
    input.panDelta = float2(0.0f);
    input.zoomDelta = 0.0f;
    input.clickedControl = 0u;
    input.nextControlId = 1u;
    input.pressedKeyCount = 0u;
    input.clickableRegions = std::move(input.nextClickableRegions);
    input.nextClickableRegions.clear();
    Examples_BeginControls(input);
}

void Examples_BeginControls(ExampleInputState& input, int x, int y)
{
    input.uiX = x;
    input.uiY = y;
    input.uiStartX = x;
    input.uiLineHeight = kExampleUiCharHeight * 2 + input.uiYSpacing;
    input.uiLastItemWidth = 0;
    input.uiLastItemHeight = 0;
    input.uiLastItemX = x;
    input.uiLastItemY = y;
    input.uiSameLine = false;
    input.hud = {};
    input.hudCount = 0;
}

void Examples_SameLine(ExampleInputState& input, int spacing)
{
    input.uiSameLine = true;
    input.uiSameLineSpacing = spacing;
}

void Examples_Text(ExampleInputState& input, StringView text)
{
    RenderUtils::CSDebugTextData& line = NextHudLine(input);
    PlaceControl(input, line);
    line.SetText(text);
    line.col32 = kExampleUiDefaultColor;
    AdvanceControl(input, line);
}

bool Examples_Button(ExampleInputState& input, StringView text)
{
    RenderUtils::CSDebugTextData& line = NextHudLine(input);
    PlaceControl(input, line);
    line.SetText(fmt::format(" {} ", text));
    const uint32_t id = input.nextControlId++;
    line.col32 = input.activeControl == id ? kExampleUiActiveColor : kExampleUiDefaultColor;
    RegisterControl(input, id, line);
    AdvanceControl(input, line);
    return input.clickedControl == id;
}

bool Examples_Slider(ExampleInputState& input, StringView label, float& value, float minValue, float maxValue,
                     float step, const char* unit, bool drag)
{
    constexpr int kBarSegments = 10;
    if (minValue > maxValue)
        std::swap(minValue, maxValue);
    const float range = std::max(maxValue - minValue, 1e-6f);
    value = std::clamp(value, minValue, maxValue);

    bool changed = false;
    if (Examples_Button(input, "[-]"))
    {
        value = std::clamp(value - step, minValue, maxValue);
        changed = true;
    }
    Examples_SameLine(input, 8);

    // The bar itself is a clickable/draggable region: while it's the active control, the pointer's
    // fractional x-position along it sets the value directly (click-to-jump and drag-to-scrub).
    const int filled = static_cast<int>(std::round(((value - minValue) / range) * kBarSegments));
    char bar[kBarSegments + 1]{};
    for (int i = 0; i < kBarSegments; ++i)
        bar[i] = i < filled ? '=' : '-';
    RenderUtils::CSDebugTextData& barLine = NextHudLine(input);
    auto text = fmt::format(" {} [{}] {:.2f}{}", label, bar, value, unit);
    if (drag)
    {
        PlaceControl(input, barLine);
        barLine.SetText(text);
        const uint32_t barId = input.nextControlId++;
        barLine.col32 = input.activeControl == barId ? kExampleUiActiveColor : kExampleUiDefaultColor;
        RegisterControl(input, barId, barLine);
        if (input.activeControl == barId && input.pointerDown)
        {
            float scrubbed = minValue + PointerFractionForLine(input, barLine) * range;
            if (step > 1e-6f)
                scrubbed = minValue + std::round((scrubbed - minValue) / step) * step;
            value = std::clamp(scrubbed, minValue, maxValue);
            changed = true;
        }
        AdvanceControl(input, barLine);
    }
    else
    {
        Examples_Text(input, text);
    }
    Examples_SameLine(input, 8);
    if (Examples_Button(input, "[+]"))
    {
        value = std::clamp(value + step, minValue, maxValue);
        changed = true;
    }
    return changed;
}

Span<const RenderUtils::CSDebugTextData> Examples_HudLines(ExampleInputState const& input)
{
    return Span<const RenderUtils::CSDebugTextData>(input.hud.data(), input.hud.size());
}

const char* Examples_GPUSceneModeName(ExampleGPUSceneRenderMode mode)
{
    return mode == ExampleGPUSceneRenderMode::PathTracer ? "Path Tracer" : "Raster";
}

void Examples_GPUSceneToggleMode(ExampleGPUSceneRenderState& state)
{
    state.mode = state.mode == ExampleGPUSceneRenderMode::Raster ? ExampleGPUSceneRenderMode::PathTracer
                                                                 : ExampleGPUSceneRenderMode::Raster;
}

void Examples_GPUSceneBuildRenderGraph(Renderer* renderer, RendererUBO* ubo, GPUScene* gpu,
                                       ExampleGPUSceneRenderState& state, ExampleInputState const& input,
                                       RHIExtent2D swapchainExtent)
{
    state.renderScale = std::clamp(state.renderScale, 0.10f, 1.0f);
    state.config.renderExtent = uint2(float2(swapchainExtent) * state.renderScale);
    state.config.renderExtent.x = std::max(state.config.renderExtent.x, 1u);
    state.config.renderExtent.y = std::max(state.config.renderExtent.y, 1u);
    state.config.ptRenderPaused = &state.renderPaused;
    state.config.rasterEffects = Span<const RasterEffect>(
        kExampleRasterEffects, sizeof(kExampleRasterEffects) / sizeof(kExampleRasterEffects[0]));

    const bool pathTracer = state.mode == ExampleGPUSceneRenderMode::PathTracer;
    renderer->BeginSetup();
    if (pathTracer)
        BuildPathTracerRenderGraph(renderer, ubo, gpu, state.config, state.outputs);
    else
        BuildRasterRenderGraph(renderer, ubo, gpu, state.config, state.outputs);
    Examples_InsertBasicTonemapPasses(renderer, state.outputs, pathTracer);
    RenderUtils::createCSDebugTextPassBackBuffer(renderer, "Debug Text", Examples_HudLines(input));
    renderer->EndSetup();
    state.renderExtent = swapchainExtent;
}

void Examples_GPUSceneFillCameraUBO(RendererUBO& ubo, Renderer* renderer, FExampleOrbitCamera const& camera,
                                    RendererConfig const& config)
{
    UpdateRendererCameraUBO(ubo, renderer->GetFrame(), camera.view, camera.proj);
    ubo.zNear = camera.zNear;
    ubo.projPlanes = planeSymmetric(camera.proj);
    ubo.camPosition = float4(camera.position, 0.0f);
    ubo.camDirection = float4(camera.rot * float3(0, 0, -1), 0.0f);
    ubo.dbgViewFlags = config.viewFlags;
    ubo.dbgMaterialFlags = config.materialFlags;
}

void Examples_NewFrame(Renderer* renderer)
{
    renderer->BeginExecute();
    renderer->ExecuteFrame();
    renderer->EndExecute();
}

bool Examples_NewFrame(SDL_Window* window, Renderer* renderer, Presenter* presenter, RHIDeviceScopedHandle<RHISurface>& surface, RHIDeviceScopedHandle<RHISwapchain>& swapchain)
{
    try
    {
        renderer->BeginExecute(presenter);
        renderer->ExecuteFrame();
        renderer->EndExecute();
        presenter->Present(renderer->GetRenderCompleteSemaphore().Get());
        return true;
    }
    catch (RHISwapchainResizeException&)
    {
        LOG(Examples, LogWarn, "Swapchain invalidated; recreating presentation surface");
        try
        {
            RHIDevice* device = swapchain.mFactory;
            swapchain.Reset();
            surface.Reset();
            if (Examples_CreateSwapchain(window, device, surface, swapchain))
                renderer->SetSwapchain(swapchain);
        }
        catch (std::exception const& e)
        {
            LOG(Examples, LogWarn, "Swapchain recreation deferred: {}", e.what());
        }
        return false;
    }
}

void Examples_DestroyVulkan(SDL_Window* window, Renderer* renderer, VulkanApplication* app,
                            RHIApplicationScopedHandle<RHIDevice>& device,
                            RHIDeviceScopedHandle<RHISurface>& surface,
                            RHIDeviceScopedHandle<RHISwapchain>& swapchain)
{
    if (device)
        device->WaitIdle();

    auto& psoCacheContexts = PipelineCacheContexts();
    auto psoCacheIt = std::ranges::find_if(psoCacheContexts,
        [renderer, device = device.Get()](PipelineCacheContext const& context)
        {
            return context.renderer == renderer || context.device == device;
        });
    if (psoCacheIt != psoCacheContexts.end())
        SavePipelineCache(*psoCacheIt->cache.Get(), psoCacheIt->path, GLOBAL_ALLOC);

    Destruct(GLOBAL_ALLOC, renderer);
    if (psoCacheIt != psoCacheContexts.end())
        psoCacheContexts.erase(psoCacheIt);
    swapchain.Reset();
    surface.Reset();
    device.Reset();
    if (app)
        Destruct(GLOBAL_ALLOC, app);
    if (window)
        SDL_DestroyWindow(window);
}

void Examples_DumpAndOpenImage(StringView path, RHIExtent2D extent, void const* data, int channels, int strideBytes)
{
    const auto outPath = std::filesystem::absolute(std::string(path.data(), path.size())).string();
    const int stride = strideBytes ? strideBytes : static_cast<int>(extent.x) * channels;
    if (!stbi_write_png(outPath.c_str(), static_cast<int>(extent.x), static_cast<int>(extent.y),
                        channels, data, stride))
    {
        LOG(Examples, LogError, "stbi_write_png failed for '{}'", outPath);
        return;
    }
    LOG(Examples, LogInfo, "Wrote '{}'", outPath);
    if (!SDL_OpenURL(outPath.c_str()))
        LOG(Examples, LogWarn, "SDL_OpenURL failed: {}", SDL_GetError());
}

ResourceHandle Examples_InsertBasicTonemapPasses(Renderer* renderer, RendererOutputs const& outputs,
                                                 bool isPathTracer)
{
    CHECK_MSG(outputs.diffuse != kInvalidHandle, "Basic tonemap pass missing diffuse output");
    RHIExtent2D extent = outputs.extent;
    if (extent.x == 0u || extent.y == 0u)
    {
        CHECK_MSG(renderer->IsPresentEnabled(),
                  "Basic tonemap pass requires outputs.extent when running headlessly");
        extent = renderer->GetSwapchainExtent();
    }
    const uint32_t w = extent.x;
    const uint32_t h = extent.y;
    constexpr RHIResourceFormat kOutputFormat = RHIResourceFormat::R8G8B8A8Unorm;

    auto postprocess = renderer->CreateResource(
        "Basic Postprocess",
        RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                  RHITextureUsageBits::SampledImage |
                                  RHITextureUsageBits::TransferSource,
                       .extent = {w, h, 1},
                       .format = kOutputFormat});

    using namespace RenderUtils;
    const char* shader = isPathTracer ? "Data/Shaders/PostprocessBasicPT.spv"
                                      : "Data/Shaders/PostprocessBasic.spv";
    createPSFullscreenPassRTV(
        renderer, "Basic Tonemap", postprocess,
        RHITextureViewDesc{.format = kOutputFormat, .range = RHITextureSubresourceRange::Create()},
        {w, h},
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", PathsResolve(shader));
            r->BindTextureSRV(self, outputs.diffuse, "diffuseTex", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = outputs.aovFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            if (isPathTracer)
            {
                const ResourceHandle specular =
                    outputs.specular != kInvalidHandle ? outputs.specular : outputs.diffuse;
                r->BindTextureSRV(self, specular, "specularTex", RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = outputs.aovFormat,
                                                     .range = RHITextureSubresourceRange::Create()});
            }
        },
        [](PassHandle, Renderer*, RHICommandList*) {});

    if (renderer->IsPresentEnabled())
    {
        const auto sampler = renderer->CreateSampler({});
        createPSBackbufferBlitPass(renderer, "Basic Tonemap Blit", sampler, postprocess, kOutputFormat);
    }
    return postprocess;
}

float Examples_GetTime()
{
    return static_cast<float>(SDL_GetTicks() / 1e3);
}

bool FExampleOrbitCamera::Update(ExampleInputState const& input, float dt)
{
    bool updated = false;
    vec3 moveDir(0.0f);
    vec3 forward = rot * vec3(0, 0, -1);
    vec3 right = rot * vec3(1, 0, 0);
    if (std::abs(input.move.y) > 1e-6f)
        moveDir += forward * input.move.y;
    if (std::abs(input.move.x) > 1e-6f)
        moveDir += right * input.move.x;
    if (glm::dot(moveDir, moveDir) > 1e-6f)
    {
        moveDir = glm::normalize(moveDir);
        center += moveDir * (moveSpeed * (input.keyFast ? 4.0f : 1.0f) * dt);
        updated = true;
    }

    if (glm::dot(input.orbitDelta, input.orbitDelta) > 1e-6f)
    {
        float yawDelta = -input.orbitDelta.x * 1e-2f;
        float pitchDelta = -input.orbitDelta.y * 1e-2f;
        quat yawRot = angleAxis(yawDelta, vec3(0, 1, 0));
        quat pitchRot = angleAxis(pitchDelta, vec3(1, 0, 0));
        rot = normalize(yawRot * rot * pitchRot);
        updated = true;
    }

    if (glm::dot(input.panDelta, input.panDelta) > 1e-6f)
    {
        vec3 up = rot * vec3(0, 1, 0);
        center -= right * (input.panDelta.x * radius * 1e-3f);
        center += up * (input.panDelta.y * radius * 1e-3f);
        updated = true;
    }

    if (std::abs(input.zoomDelta) > 1e-6f)
    {
        radius -= input.zoomDelta * radius * 1e-1f;
        radius = radius < 1e-3f ? 1e-3f : radius;
        updated = true;
    }

    RefreshMatrices();
    return updated;
}

void FExampleOrbitCamera::RefreshMatrices()
{
    proj = infinitePerspectiveRHReverseZ(fovY, aspect, zNear);
    vec3 dir = rot * vec3(0, 0, 1);
    position = center + radius * dir;
    view = viewMatrixRHReverseZ(position, rot);
}
