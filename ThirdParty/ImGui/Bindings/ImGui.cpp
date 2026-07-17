#include "ImGui.hpp"
#include "Fonts/PlexSansIcon.h"
#include <filesystem>
#include <Core/Paths.hpp>
#include "tracy/Tracy.hpp"

#include <RenderCore/Bindless.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include <SDL3/SDL.h>
#include <imgui_impl_sdl3.h>

using namespace Foundation;
using namespace RenderCore;
using namespace Core;
using namespace RHI;
using namespace Math;

constexpr size_t kMaxTextures = 1024;
constexpr size_t kUploadBudget = 16 * (1u << 20);
// NOTE: Should be large enough to accommodate most frames, or there will be a race.
constexpr size_t kVertexBufferSize = 16 * (1u << 20);
constexpr size_t kIndexBufferSize = 16 * (1u << 20);

UniquePtr<BindlessPool> gImGuiTexturePool = nullptr;
void* ImGui_ImplFoundation_MemAlloc(size_t sz, void*) { return GLOBAL_ALLOC->Allocate(sz, sizeof(std::max_align_t)); }
void ImGui_ImplFoundation_MemFree(void* ptr, void*) { return GLOBAL_ALLOC->Deallocate(ptr); }
struct ImGuiImplFoundationBd
{
    RHIDevice* device;
    size_t vtxBufferOffset = 0;
    size_t idxBufferOffset = 0;
} gBackendData;
void ImGui_ImplFoundation_Init(RHIDevice* device, SDL_Window* window)
{
    // Reference being the official Vulkan implementation - sans Viewport support to keep things _really_ simple
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    gBackendData.device = device;
    gBackendData.vtxBufferOffset = gBackendData.idxBufferOffset = 0;
    io.BackendRendererUserData = static_cast<void*>(&gBackendData);
    io.BackendRendererName = "imgui_impl_foundation";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // We can honor the ImDrawCmd::VtxOffset field,
                                                               // allowing for large meshes.
    io.BackendFlags |=
        ImGuiBackendFlags_RendererHasTextures; // We can honor ImGuiPlatformIO::Textures[] requests during render.
    gImGuiTexturePool = ConstructUnique<BindlessPool>(GLOBAL_ALLOC, device, GLOBAL_ALLOC,
                                                      BindlessPool::BindlessPoolDesc{.maxBindings = kMaxTextures});
    // Init windowing backend
    ImGui_ImplSDL3_InitForOther(window);
}
void ImGui_ImplFoundation_ProcessEvent(SDL_Event* event)
{
    ImGui_ImplSDL3_ProcessEvent(event);
}
void ImGui_ImplFoundation_NewFrame()
{
    ImGui_ImplSDL3_NewFrame();
}
void ImGui_ImplFoundation_Shutdown() { gImGuiTexturePool.reset(); }
// Tagged pointer.
Pair<uint32_t, ImGui_ImplFoundation_ImageSampler> ImGui_ImplFoundation_DecodeImTextureID(ImTextureID id)
{
    uint32_t handle = (id & ~0u) - 1; // 0 reserved
    size_t sampler = id >> (sizeof(uint32_t) * 8);
    return {handle, static_cast<ImGui_ImplFoundation_ImageSampler>(sampler)};
}
ImTextureID ImGui_ImplFoundation_EncodeImTextureID(
    uint32_t handle, ImGui_ImplFoundation_ImageSampler sampler = ImGuiImplFoundationImageSamplerLinear)
{
    ImTextureID id = handle + 1; // 0 reserved
    size_t smp = static_cast<ImTextureID>(sampler);
    smp = smp << (sizeof(uint32_t) * 8);
    id = id | smp;
    return id;
}
void ImGui_ImplFoundation_ImplUpdateTexture(ImTextureData* tex)
{
    CHECK_MSG(gImGuiTexturePool, "Backend not initialized. Did you call ImGui_ImplFoundation_Init()?");
    auto* device = static_cast<ImGuiImplFoundationBd*>(ImGui::GetIO().BackendRendererUserData)->device;
    if (tex->Status == ImTextureStatus_OK)
        return;
    // Allocation
    if (tex->Status == ImTextureStatus_WantCreate)
    {
        IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
        auto hdl = device->CreateTexture(
            {.usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
             .extent = {tex->Width, tex->Height, 1},
             .format = RHIResourceFormat::R8G8B8A8Unorm});
        auto view = hdl->CreateTextureView(
            {.format = RHIResourceFormat::R8G8B8A8Unorm, .range = RHITextureSubresourceRange::Create()});
        auto handle = gImGuiTexturePool->Allocate(std::move(hdl), std::move(view));
        tex->SetTexID(ImGui_ImplFoundation_EncodeImTextureID(handle));
        tex->BackendUserData = gImGuiTexturePool.get();
    }
    // Actually updating the texture
    // XXX: Slow path. See comment below.
    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
    {
        const int upload_x = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.x;
        const int upload_y = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.y;
        const int upload_w = (tex->Status == ImTextureStatus_WantCreate) ? tex->Width : tex->UpdateRect.w;
        const int upload_h = (tex->Status == ImTextureStatus_WantCreate) ? tex->Height : tex->UpdateRect.h;
        size_t upload_pitch = upload_w * tex->BytesPerPixel;
        auto [hdl, smp] = ImGui_ImplFoundation_DecodeImTextureID(tex->GetTexID());
        RHITexture* texture = gImGuiTexturePool->GetResource(hdl);
        auto staging = device->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Upload,
                                                          .hostAccess = RHIResourceHostAccess::WriteOnly,
                                                          .staging = true},
                                             .usage = RHIBufferUsageBits::TransferSource,
                                             .size = kUploadBudget});
        auto dst = static_cast<char*>(staging->Map());
        for (int y = 0; y < upload_h; y++)
            std::memcpy(dst + upload_pitch * y, tex->GetPixelsAt(upload_x, upload_y + y), upload_pitch);
        ImmediateContext im(RHIDeviceQueueType::Graphics, device);
        im->Begin();
        // Transition
        im->BeginTransition();
        im->SetImageTransition(texture,
                               {
                                   .dstAccess = RHIResourceAccessBits::TransferWrite,
                                   .dstStage = RHIPipelineStageBits::Transfer,
                                   .dstImgLayout = RHITextureLayout::TransferDst,
                                   .srcImgRange = RHITextureSubresourceRange::Create(),
                               });
        im->EndTransition();
        // Copy
        im->CopyBufferToImage(staging.Get(), texture, RHITextureLayout::TransferDst,
                              {{RHICommandList::CopyImageRegion{
                                  .srcBufferOffset = 0,
                                  .dstLayer =
                                      RHITextureSubresourceLayer{
                                          .aspect = RHITextureAspectFlagBits::Color,
                                          .mipLevel = 0,
                                          .baseArrayLayer = 0,
                                          .layerCount = 1,
                                      },
                                  .dstOffset = {upload_x, upload_y, 0},
                                  .extent = {static_cast<uint32_t>(upload_w), static_cast<uint32_t>(upload_h), 1}}}});
        // Transition to ShaderReadOnly
        im->BeginTransition();
        im->SetImageTransition(texture,
                               {
                                   .srcAccess = RHIResourceAccessBits::TransferWrite,
                                   .dstAccess = RHIResourceAccessBits::ShaderRead,
                                   .srcStage = RHIPipelineStageBits::Transfer,
                                   .dstStage = RHIPipelineStageBits::FragmentShader,
                                   .srcImgLayout = RHITextureLayout::TransferDst,
                                   .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                                   .srcImgRange = RHITextureSubresourceRange::Create(),
                               });
        im->EndTransition();
        im->End();
        im.Submit();
        // XXX: Wait for device.
        // ImTextureStatus doesn't offer in-progress states, so unfortunately async uploads isn't really possible.
        // Texture updates *here* are expected to be only font atlas updates, however.
        // Plus how font atlas build process itself is synchronous, we'll leave it like this for now.
        im.WaitIdle();
        tex->SetStatus(ImTextureStatus_OK);
    }
    // Release
    if (tex->Status == ImTextureStatus_WantDestroy)
    {
        ImGui_ImplFoundation_RemoveImage(tex->GetTexID());
        tex->SetStatus(ImTextureStatus_Destroyed);
        tex->SetTexID(0);
        tex->BackendUserData = nullptr;
    }
}
ImTextureID ImGui_ImplFoundation_AddImage(RHITextureView* textureView, ImGui_ImplFoundation_ImageSampler sampler)
{
    CHECK_MSG(gImGuiTexturePool, "Backend not initialized. Did you call ImGui_ImplFoundation_Init()?");
    uint32_t handle = gImGuiTexturePool->Allocate(textureView);
    return ImGui_ImplFoundation_EncodeImTextureID(handle, sampler);
}
void ImGui_ImplFoundation_RemoveImage(ImTextureID textureID)
{
    CHECK_MSG(gImGuiTexturePool, "Backend not initialized. Did you call ImGui_ImplFoundation_Init()?");
    auto [hdl, smp] = ImGui_ImplFoundation_DecodeImTextureID(textureID);
    gImGuiTexturePool->Free(hdl);
}
#pragma pack(push, 4)
struct PushConstants
{
    float2 s; // scale
    float2 t; // translation
    uint textureId;
    uint samplerId;
};
#pragma pack(pop)

void ImGui_ImplFoundation_ImplPassSetup(PassHandle self, Renderer* r, ResourceHandle vtxBuffer,
                                        ResourceHandle idxBuffer, ResourceHandle linSampler,
                                        ResourceHandle nearSampler)
{
    r->BindBackbufferRTV(self, RHIPipelineState::PipelineStateDesc::Attachment::Blending::GetAlphaBlending());
    r->BindBufferCopyDst(self, vtxBuffer);
    r->BindBufferCopyDst(self, idxBuffer);
    r->BindPushConstant(self, RHIShaderStageBits::Vertex, 0, sizeof(PushConstants));
    // Specialization constant
    uint flags{};
    // We can output to these colorspaces:
    const int kFlagsOutputSrgb = 1 << 0;
    const int kFlagsOutputSt2084 = 1 << 1;
    // As for formats, disallow sRGB backbuffers since these doesn't make scene for UI elements *and* we'd always tonemap to sRGB space
    // (output in SDR therefore *requires* UNORM backbuffers)
    // HDR-wise we'd only support ST2084/PQ and it happily takes RGB10A2 formats.
    CHECK_MSG(!IsFormatSRGB(r->GetSwapchain()->mDesc.format), "sRGB backbuffers are not supported by ImGui. Please use UNORM formats instead.");
    switch (r->GetSwapchain()->mDesc.colorSpace)
    {       
    case RHIColorSpace::Hdr10St2084:
        flags |= kFlagsOutputSt2084;
        break;
    case RHIColorSpace::SrgbNonLinear:
    default:
        flags |= kFlagsOutputSrgb;
        break;
    }
    r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", Foundation::Core::PathsResolve("Data/Shaders/ImGui.spv"), AsBytes(AsSpan(flags)));
    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Foundation::Core::PathsResolve("Data/Shaders/ImGui.spv"), AsBytes(AsSpan(flags)));
    r->BindDescriptorSet(self, "textures", gImGuiTexturePool->GetDescriptorSetLayout());
    // We have fixed samplers for ImGui - quite enough for UI elements
    r->BindTextureSampler(self, linSampler, "linSampler");
    r->BindTextureSampler(self, nearSampler, "nearSampler");
    r->BindVertexInput(
        self,
        {.bindings = {{{sizeof(ImDrawVert), false}}},
         .attributes = {{
             {.location = 0, .offset = offsetof(ImDrawVert, pos), .format = RHIResourceFormat::R32G32SignedFloat},
             {.location = 1, .offset = offsetof(ImDrawVert, uv), .format = RHIResourceFormat::R32G32SignedFloat},
             {.location = 2, .offset = offsetof(ImDrawVert, col), .format = RHIResourceFormat::R8G8B8A8Unorm},
         }}});
    // No culling
    r->PassSetRasterizerFlags(self, {.cullMode = RHIPipelineState::PipelineStateDesc::Rasterizer::CullNone});    
}
void ImGui_ImplFoundation_ImplPassRecord(PassHandle self, Renderer* r, bool clear, RHICommandList* cmd,
                                         ResourceHandle vtxBuffer, ResourceHandle idxBuffer)
{
    auto const& img_wh = r->GetSwapchainExtent();
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    // Upload textures
    if (draw_data->Textures != nullptr)
        for (ImTextureData* tex : *draw_data->Textures)
            if (tex->Status != ImTextureStatus_OK)
                ImGui_ImplFoundation_ImplUpdateTexture(tex);
    // Upload vertex/index buffers
    auto* vtx = r->DerefResource(vtxBuffer).Get<RHIBuffer*>();
    auto* idx = r->DerefResource(idxBuffer).Get<RHIBuffer*>();
    // Map. We're using a ring buffer here.
    auto* bd = static_cast<ImGuiImplFoundationBd*>(ImGui::GetIO().BackendRendererUserData);
    // Bump or rewind
    {
        int vtxBufferSize = kVertexBufferSize - bd->vtxBufferOffset, idxBufferSize = kIndexBufferSize - bd->idxBufferOffset;
        for (const ImDrawList* draw_list : draw_data->CmdLists)
        {
            vtxBufferSize -= draw_list->VtxBuffer.size() * sizeof(ImDrawVert);
            idxBufferSize -= draw_list->IdxBuffer.size() * sizeof(ImDrawIdx);
        }
        if (vtxBufferSize < 0)
            bd->vtxBufferOffset = 0;
        if (idxBufferSize < 0)
            bd->idxBufferOffset = 0;
    }
    auto pVtx = vtx->Map<char>() + bd->vtxBufferOffset;
    auto pIdx = idx->Map<char>() + bd->idxBufferOffset;
    auto* vtx_dst = reinterpret_cast<ImDrawVert*>(pVtx);
    auto* idx_dst = reinterpret_cast<ImDrawIdx*>(pIdx);
    size_t vtx_offset = bd->vtxBufferOffset, idx_offset = bd->idxBufferOffset;
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        std::memcpy(vtx_dst, draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
        std::memcpy(idx_dst, draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
        bd->vtxBufferOffset += draw_list->VtxBuffer.size() * sizeof(ImDrawVert);
        bd->idxBufferOffset += draw_list->IdxBuffer.size() * sizeof(ImDrawIdx);
        vtx_dst += draw_list->VtxBuffer.Size;
        idx_dst += draw_list->IdxBuffer.Size;
    }
    vtx->Flush(), idx->Flush();
    // Implementations guarantee that mapped, flushed resources are available at
    // the time of the next device queue submit - so extra barriers are not needed.
    r->CmdSetPipeline(self, cmd);
    r->CmdBindDescriptorSet(self, cmd, "textures", gImGuiTexturePool->GetDescriptorSet());
    r->CmdBeginGraphics(self, cmd, img_wh, {{{clear ? RHIAttachmentLoadOp::Clear : RHIAttachmentLoadOp::Load}}});
    // Setup states
    int fb_width = img_wh.x, fb_height = img_wh.y;
    cmd->SetViewport(0, 0, fb_width, fb_height); // Full screen
    cmd->BindVertexBuffer(0, {{vtx}}, {{vtx_offset}});
    cmd->BindIndexBuffer(idx, idx_offset, RHIResourceFormat::R16Uint);
    // Setup scale and translation:
    // Our visible imgui space lies from draw_data->DisplayPps (top left) to
    // draw_data->DisplayPos+data_data->DisplaySize (bottom right). DisplayPos is (0,0) for single viewport apps.
    PushConstants pc{
        .s = {2.0f / draw_data->DisplaySize.x, 2.0f / draw_data->DisplaySize.y},
        .t = {-1.0f - draw_data->DisplayPos.x * (2.0f / draw_data->DisplaySize.x),
              -1.0f - draw_data->DisplayPos.y * (2.0f / draw_data->DisplaySize.y)},
        .textureId = 0 // Updated per-draw
    };
    // Render command list
    // Will project scissor/clipping rectangles into framebuffer space
    ImVec2 clip_off = draw_data->DisplayPos; // (0,0) unless using multi-viewports
    ImVec2 clip_scale = draw_data->FramebufferScale; // (1,1) unless using retina display which are often (2,2)
    int global_vtx_offset = 0, global_idx_offset = 0;
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
            // if (pcmd->UserCallback != nullptr) /* No Support */
            // Project scissor/clipping rectangles into framebuffer space
            ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x,
                            (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
            ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x,
                            (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);
            // Clamp to viewport as vkCmdSetScissor() won't accept values that are off bounds
            clip_min.x = std::clamp(clip_min.x, 0.0f, static_cast<float>(fb_width));
            clip_min.y = std::clamp(clip_min.y, 0.0f, static_cast<float>(fb_height));
            if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                continue;
            cmd->SetScissor(clip_min.x, clip_min.y, clip_max.x - clip_min.x, clip_max.y - clip_min.y);
            // All textures live in the @ref TexturePool - akin to D3D12's ResourceDescriptorHeap
            // Whether they exist or not - push it
            auto [textureId, samplerId] = ImGui_ImplFoundation_DecodeImTextureID(pcmd->GetTexID());
            pc.textureId = textureId;
            switch (samplerId)
            {
            case ImGuiImplFoundationImageSamplerLinear:
                pc.samplerId = 0;
                break;
            case ImGuiImplFoundationImageSamplerNearest:
                pc.samplerId = 1;
                break;
            }
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex, 0, pc);
            // Draw!
            cmd->DrawIndexed(pcmd->ElemCount, 1, pcmd->IdxOffset + global_idx_offset,
                             pcmd->VtxOffset + global_vtx_offset, 0);
        }
        global_idx_offset += draw_list->IdxBuffer.Size;
        global_vtx_offset += draw_list->VtxBuffer.Size;
    }
    cmd->EndGraphics();
}
void ImGui_ImplFoundation_ImplCreateResources(Renderer* renderer, ResourceHandle& outVtxBuffer,
                                              ResourceHandle& outIdxBuffer,
                                              ResourceHandle& outLinearSampler, ResourceHandle& outNearestSampler)
{
    outVtxBuffer = renderer->CreateResource(
        "ImGui Vertex Buffer",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly},
                      .usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = kVertexBufferSize});
    outIdxBuffer = renderer->CreateResource(
        "ImGui Index Buffer",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly},
                      .usage = RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = kIndexBufferSize});
    outLinearSampler = renderer->CreateSampler({});
    outNearestSampler =
        renderer->CreateSampler({.filter = {.minFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor,
                                            .magFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor}});
}
void ImGui_ImplFoundation_SetupContextWithDefaultStyles()
{
    ImGui::SetAllocatorFunctions(ImGui_ImplFoundation_MemAlloc, ImGui_ImplFoundation_MemFree);
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking
    io.ConfigDpiScaleFonts = true; // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor
                                   // DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
    io.Fonts->Clear();
    io.Fonts->AddFontFromMemoryCompressedBase85TTF(kEmbedFontPlexSansIcon, 15.0f);
    ImGuiStyle& style = ImGui::GetStyle();
    style.CircleTessellationMaxError = 0.01f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(10.0f, 7.0f);
    style.CellPadding = ImVec2(8.0f, 6.0f);
    style.ItemSpacing = ImVec2(7.0f, 9.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 16.0f;
    style.GrabMinSize = 12.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.TabBorderSize = 1.0f;
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.TabRounding = 4.0f;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);

    ImVec4* colors = style.Colors;
    const ImVec4 accentColor = ImVec4(0.68f, 0.18f, 0.16f, 1.00f);
    float accentH, accentS, accentV;
    ImGui::ColorConvertRGBtoHSV(accentColor.x, accentColor.y, accentColor.z, accentH, accentS, accentV);
    auto saturate = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    auto accent = [&](float saturationScale, float valueScale, float alpha = 1.0f) {
        ImVec4 result;
        ImGui::ColorConvertHSVtoRGB(accentH, saturate(accentS * saturationScale), saturate(accentV * valueScale),
                                    result.x, result.y, result.z);
        result.w = alpha;
        return result;
    };
    colors[ImGuiCol_Text] = ImVec4(0.78f, 0.78f, 0.78f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.42f, 0.42f, 1.00f);
    colors[ImGuiCol_WindowBg] = ImVec4(0.20f, 0.20f, 0.19f, 1.00f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.20f, 0.20f, 0.19f, 1.00f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.16f, 0.15f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.05f, 0.05f, 0.05f, 0.90f);
    colors[ImGuiCol_BorderShadow] = accent(1.0f, 0.91f, 0.18f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.19f, 0.19f, 0.18f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.27f, 0.27f, 0.26f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.16f, 0.15f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.25f, 0.25f, 0.24f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.16f, 0.16f, 0.15f, 1.00f);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.18f, 0.18f, 0.17f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.12f, 0.12f, 0.11f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.31f, 0.30f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.38f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.48f, 0.48f, 0.46f, 1.00f);
    colors[ImGuiCol_CheckMark] = accent(1.0f, 1.0f);
    colors[ImGuiCol_SliderGrab] = accent(1.0f, 0.62f);
    colors[ImGuiCol_Button] = ImVec4(0.22f, 0.22f, 0.21f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.24f, 0.24f, 0.23f, 1.00f);
    colors[ImGuiCol_Separator] = ImVec4(0.09f, 0.09f, 0.09f, 0.85f);
    colors[ImGuiCol_ResizeGrip] = accent(1.0f, 0.62f, 0.35f);
    colors[ImGuiCol_ResizeGripHovered] = accent(1.0f, 0.76f, 0.65f);
    colors[ImGuiCol_ResizeGripActive] = accent(1.0f, 0.96f, 0.90f);
    colors[ImGuiCol_InputTextCursor] = accent(1.0f, 1.18f);
    colors[ImGuiCol_Tab] = ImVec4(0.20f, 0.20f, 0.19f, 1.00f);
    colors[ImGuiCol_TabHovered] = ImVec4(0.34f, 0.34f, 0.32f, 1.00f);
    colors[ImGuiCol_TabSelected] = ImVec4(0.27f, 0.27f, 0.25f, 1.00f);
    colors[ImGuiCol_TabSelectedOverline] = accent(1.0f, 0.85f);
    colors[ImGuiCol_TabDimmed] = ImVec4(0.15f, 0.15f, 0.14f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.23f, 0.23f, 0.22f, 1.00f);
    colors[ImGuiCol_TabDimmedSelectedOverline] = accent(1.0f, 0.62f);
    colors[ImGuiCol_DockingPreview] = accent(1.0f, 0.91f, 0.45f);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.15f, 0.15f, 0.14f, 1.00f);
    colors[ImGuiCol_PlotLines] = accent(1.0f, 1.06f);
    colors[ImGuiCol_PlotLinesHovered] = accent(1.0f, 1.29f);
    colors[ImGuiCol_PlotHistogram] = accent(1.0f, 0.76f);
    colors[ImGuiCol_PlotHistogramHovered] = accent(1.0f, 1.09f);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.22f, 0.22f, 0.21f, 1.00f);
    colors[ImGuiCol_TableBorderStrong] = ImVec4(0.08f, 0.08f, 0.08f, 1.00f);
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.04f);
    colors[ImGuiCol_TextLink] = accent(1.0f, 1.06f);
    colors[ImGuiCol_TextSelectedBg] = accent(1.0f, 0.85f, 0.35f);
    colors[ImGuiCol_TreeLines] = accent(1.0f, 0.62f, 0.55f);
    colors[ImGuiCol_DragDropTarget] = ImVec4(0.85f, 0.80f, 0.45f, 0.90f);
    colors[ImGuiCol_NavCursor] = accent(1.0f, 1.10f);
    colors[ImGuiCol_NavWindowingHighlight] = accent(1.0f, 1.26f, 0.70f);
    colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.35f);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.55f);
    colors[ImGuiCol_ButtonActive] = colors[ImGuiCol_HeaderActive] = colors[ImGuiCol_SliderGrabActive] =
    colors[ImGuiCol_SeparatorActive] = accent(1.0f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = colors[ImGuiCol_HeaderHovered] = colors[ImGuiCol_SeparatorHovered] =
        accent(1.0f, 0.75f);
}
