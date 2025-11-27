#pragma once
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#define IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DISABLE_DEFAULT_ALLOCATORS
#include <SDL3/SDL.h>
#include <imgui.h>

#include <RHICore/Device.hpp>
#include <RenderCore/Renderer.hpp>
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
void ImGui_ImplFoundation_ImplPassSetup(Foundation::RenderCore::PassHandle self,
                                        Foundation::RenderCore::Renderer* renderer,
                                        Foundation::RenderCore::ResourceHandle vtxBuffer,
                                        Foundation::RenderCore::ResourceHandle idxBuffer,
                                        Foundation::RenderCore::ResourceHandle linSampler,
                                        Foundation::RenderCore::ResourceHandle nearSampler);
void ImGui_ImplFoundation_ImplPassRecord(Foundation::RenderCore::PassHandle self,
                                         Foundation::RenderCore::Renderer* renderer, bool clear,
                                         Foundation::RHI::RHICommandList* cmd,
                                         Foundation::RenderCore::ResourceHandle vtxBuffer,
                                         Foundation::RenderCore::ResourceHandle idxBuffer);
void ImGui_ImplFoundation_ImplCreateResources(Foundation::RenderCore::Renderer* renderer,
                                              Foundation::RenderCore::ResourceHandle& outVtxBuffer,
                                              Foundation::RenderCore::ResourceHandle& outIdxBuffer,
                                              Foundation::RenderCore::ResourceHandle& outLinearSampler,
                                              Foundation::RenderCore::ResourceHandle& outNearestSampler);
// ^^^ Internals

/* -- APIs -- */
/**
 * @breif Initialize global context (@ref TexturePool, etc.) for our ImGui backend
 *        Call this once context is created, within BeginSetup/EndSetup block of your @ref Renderer.
 *
 * @param device @ref RHIDevice of the @ref Renderer
 * @param window @ref SDL_Window
 * @param clear Whether to clear the render target before drawing.
 * @param dependOn (Optional) A pass to depend on. You may want this if the prior pass writes to resources ImGui may renderer
 *                 (e.g. through ImGui_ImplFoundation_AddImage). If the prior pass writes to the backbuffer as well, this
 *                 is not required as dependency will be automatically created.
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
 * @brief Creates a render pass that will draw the ImGui UI.
 * @tparam FSetup A callable type for custom render pass setup.
 *                You may want this when ImGui is the last pass of your @ref Renderer setup,
 *                as you can declare dependencies here.
 *                See @ref ModelViewer's @ref OnRendererSetup for an example.
 *                However - if your prior pass writes to the backbuffer as well - dependency will
 *                be automatically created and this won't be required.
 * @param renderer The main renderer object.
 * @param name A name for the pass, for debugging purposes.
 * @param clear Whether to clear the render target before drawing.
 */
template <typename FSetup>
Foundation::RenderCore::PassHandle ImGui_ImplFoundation_CreatePass(Foundation::RenderCore::Renderer* renderer, Foundation::Core::StringView name, bool clear, FSetup&& setup)
{
    using namespace Foundation;
    using namespace RenderCore;
    using namespace RHI;
    ResourceHandle vtxBuffer, idxBuffer, linSampler, nearSampler;
    ImGui_ImplFoundation_ImplCreateResources(renderer, vtxBuffer, idxBuffer, linSampler, nearSampler);
    return renderer->CreatePass(
        "ImGui", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            ImGui_ImplFoundation_ImplPassSetup(self, r, vtxBuffer, idxBuffer, linSampler, nearSampler);
            setup(self, r);
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        { ImGui_ImplFoundation_ImplPassRecord(self, r, clear, cmd, vtxBuffer, idxBuffer); });
}


/**
 * @brief Applies a default, vaguely stylish theme to the ImGui context.
 * @note Call this after @ref ImGui_ImplFoundation_Init if you don't want to set up your own styles.
 */
void ImGui_ImplFoundation_SetupContextWithDefaultStyles();
// ^^^ APIs
