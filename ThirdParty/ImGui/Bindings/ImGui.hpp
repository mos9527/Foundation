#pragma once
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DISABLE_DEFAULT_ALLOCATORS
#include <SDL3/SDL.h>
#include <imgui.h>

#include <RHICore/Device.hpp>
#include <RHICore/PipelineState.hpp>
#include <RHICore/Command.hpp>

/**
 * @brief Sampler type for added image.
 */
enum ImGui_ImplFoundation_ImageSampler
{
    ImGuiImplFoundationImageSamplerLinear = 0,
    ImGuiImplFoundationImageSamplerNearest = 1
};

/* -- Internals -- */
void ImGui_ImplFoundation_ImplUpdateTexture(ImTextureData* tex);
// ^^^ Internals

/**
 * @brief Initialize global context (@ref TexturePool, etc.) for our ImGui backend
 *        Call this once context is created.
 *
 * @param device The device to use for ImGui.
 * @param window The SDL window for ImGui to attach to.
 *
 * @note You _MUST_ call @ref ImGui_ImplFoundation_Shutdown before the destruction of @ref RHIDevice related objects.
 *       See @ref Examples::ImGui or other ImGui backend usage for reference.
 */
void ImGui_ImplFoundation_Init(Foundation::RHI::RHIDevice* device, SDL_Window* window);

/**
 * @brief Processes one SDL event and forwards it to ImGui.
 * @param event The SDL event to be processed.
 * @note You should call this for each SDL event in your main event loop,
 *       i.e. while (SDL_PollEvent(&event)) { ImGui_ImplFoundation_ProcessEvent(&event); }
 */
void ImGui_ImplFoundation_ProcessEvent(SDL_Event* event);
/**
 * @brief Starts a new ImGui frame.
 * @note You should call this once per frame, before any other ImGui calls.
 *       See @ref Examples::ImGui or other ImGui backend usage for reference.
 */
void ImGui_ImplFoundation_NewFrame();

/**
 * @brief Shuts down the ImGui backend and releases all resources.
 * @note You _MUST_ call @ref ImGui_ImplFoundation_Shutdown before the destruction of @ref RHIDevice related objects.
 *       See @ref Examples::ImGui or other ImGui backend usage for reference.
 */
void ImGui_ImplFoundation_Shutdown();

/**
 * @brief Registers a texture with the ImGui backend so it can be displayed in the UI.
 * @param textureView The texture view to be used.
 * @param sampler The sampler to be used for the texture.
 * @return An ImTextureID that you can use with ImGui::Image() and other functions.
 */
ImTextureID
ImGui_ImplFoundation_AddImage(Foundation::RHI::RHITextureView* textureView,
                              ImGui_ImplFoundation_ImageSampler sampler = ImGuiImplFoundationImageSamplerLinear);

/**
 * @brief Unregisters a texture from the ImGui backend.
 * @param textureID The ID returned by @ref ImGui_ImplFoundation_AddImage.
 * @note Call this when you no longer need the texture in the UI to free up resources.
 */
void ImGui_ImplFoundation_RemoveImage(ImTextureID textureID);

/**
 * @brief Records and submits ImGui draw commands on the backend's managed command lists.
 * @param draw_data ImGui draw data to render.
 * @param rtv The render target view to render into.
 * @param clear Whether to clear the render target before drawing.
 * @param currentSwap Current frame index in flight (0 <= index < frameSwaps).
 * @param waitSemaphore Optional semaphore to wait on before execution.
 * @param waitStage Pipeline stage to wait for waitSemaphore.
 * @param signalSemaphore Optional semaphore to signal after execution finishes.
 */
void ImGui_ImplFoundation_RenderDrawData(ImDrawData* draw_data,
                                         Foundation::RHI::RHITextureView* rtv, 
                                         Foundation::RHI::RHIResourceFormat rtvFormat,
                                         Foundation::RHI::RHIColorSpace colorSpace,
                                         bool clear, uint32_t currentSwap,
                                         Foundation::RHI::RHIDeviceSemaphore* waitSemaphore = nullptr,
                                         Foundation::RHI::RHIPipelineStage waitStage = Foundation::RHI::RHIPipelineStageBits::RenderTargetOutput,
                                         Foundation::RHI::RHIDeviceSemaphore* signalSemaphore = nullptr);


/**
 * @brief Finalizes the ImGui frame and renders into the swapchain.
 *
 * This is the "one call does it all" end-of-frame function. It:
 *  1. Calls ImGui::Render() to finalize draw data.
 *  2. Records & submits ImGui draw commands, waiting on the provided
 *     waitSemaphore and signaling an internal semaphore.
 *  3. Returns the signaled semaphore for presentation.
 *
 * Semaphore management is fully internal.
 */
Foundation::RHI::RHIDeviceSemaphore* ImGui_ImplFoundation_EndFrame(
    Foundation::RHI::RHITextureView* rtv,
    Foundation::RHI::RHIResourceFormat rtvFormat,
    Foundation::RHI::RHIColorSpace colorSpace,
    uint32_t currentSwap,
    bool clear,
    Foundation::RHI::RHIDeviceSemaphore* waitSemaphore);

/**
 * @brief Applies a default, vaguely stylish theme to the ImGui context.
 * @note Call this after @ref ImGui_ImplFoundation_Init if you don't want to set up your own styles.
 */
void ImGui_ImplFoundation_SetupContextWithDefaultStyles();
// ^^^ APIs
