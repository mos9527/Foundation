#pragma once
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
#include <imgui.h>

#include <RHICore/Device.hpp>
#include <RenderCore/Renderer.hpp>
#include <Rendering/TexturePool.hpp>


void ImGui_ImplFoundation_Init(Foundation::RHI::RHIDevice* device, Foundation::Core::Allocator* allocator);
void ImGui_ImplFoundation_Shutdown();

void ImGui_ImplFoundation_UpdateTexture(ImTextureData* tex);
ImTextureID ImGui_ImplFoundation_AddImage(Foundation::RHI::RHITextureView* textureView);
void ImGui_ImplFoundation_RemoveImage(ImTextureID textureID);

void* ImGui_ImplFoundation_CreatePass(
    Foundation::RenderCore::Renderer* renderer, Foundation::Core::StringView name);

void ImGui_ImplFoundation_SetupContextWithDefaultStyles();
