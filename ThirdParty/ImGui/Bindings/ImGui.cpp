#include "ImGui.hpp"
#include <Bits/Format.hpp>
#include <Core/DefaultAllocator.hpp>
#include <Rendering/UploadContext.hpp>
#include <imgui_impl_glfw.h>
using namespace Foundation;
using namespace RenderCore;
using namespace Rendering;
using namespace Core;
using namespace RHI;
using namespace Bits;
using namespace Math;

constexpr size_t kMaxTextures = 1024;
constexpr size_t kUploadBudget = 16_MB;
constexpr size_t kVertexBufferSize = 8_MB;
constexpr size_t kIndexBufferSize = 4_MB;
const Native::Path kDefaultFontPath = "./data/assets/LXGWNeoXiHei.ttf";

UniquePtr<TexturePool> gTexturePool;
UniquePtr<UploadContext> gTextureUploadContext;
DefaultAllocator gAllocator;

void ImGui_ImplFoundation_Init(RHIDevice* device, Foundation::Native::NativeWindow* window, Allocator* allocator)
{
    // Reference being the official Vulkan implementation - sans Viewport support to keep things _really_ simple
    ImGuiIO& io = ImGui::GetIO();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");
    auto bd = device;
    io.BackendRendererUserData = static_cast<void*>(bd);
    io.BackendRendererName = "imgui_impl_foundation";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset; // We can honor the ImDrawCmd::VtxOffset field,
                                                               // allowing// for large meshes.
    io.BackendFlags |=
        ImGuiBackendFlags_RendererHasTextures; // We can honor ImGuiPlatformIO::Textures[] requests during render.
    gTexturePool = ConstructUnique<TexturePool>(allocator, device, allocator, kMaxTextures);
    gTextureUploadContext = ConstructUnique<UploadContext>(allocator, device, allocator, kUploadBudget);
    // Init windowing backend
    ImGui_ImplGlfw_InitForOther(static_cast<GLFWwindow*>(window->GetNative()), true);
}
void ImGui_ImplFoundation_NewFrame() { ImGui_ImplGlfw_NewFrame(); }
void ImGui_ImplFoundation_Shutdown()
{
    gTexturePool.reset();
    gTextureUploadContext.reset();
}

Pair<TexturePoolHandle, ImGui_ImplFoundation_ImageSampler> ImGui_ImplFoundation_DecodeImTextureID(ImTextureID id)
{
    TexturePoolHandle handle = id & kInvalidTexturePoolHandle;
    size_t sampler = id >> (sizeof(TexturePoolHandle) * 8);
    return {handle, static_cast<ImGui_ImplFoundation_ImageSampler>(sampler)};
}
ImTextureID ImGui_ImplFoundation_EncodeImTextureID(
    TexturePoolHandle handle, ImGui_ImplFoundation_ImageSampler sampler = ImGuiImplFoundationImageSamplerLinear)
{
    ImTextureID id = handle;
    size_t smp = static_cast<ImTextureID>(sampler);
    smp = smp << (sizeof(TexturePoolHandle) * 8);
    id = id | smp;
    return id;
}
void ImGui_ImplFoundation_ImplUpdateTexture(ImTextureData* tex)
{
    CHECK_MSG(gTexturePool && gTextureUploadContext,
              "Backend not initialized. Did you call ImGui_ImplFoundation_Init()?");
    if (tex->Status == ImTextureStatus_OK)
        return;
    // Allocation
    if (tex->Status == ImTextureStatus_WantCreate)
    {
        IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
        auto handle = gTexturePool->Allocate(
            RHITextureDesc{.usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
                           .extent = {tex->Width, tex->Height, 1},
                           .format = RHIResourceFormat::R8G8B8A8Unorm});
        tex->SetTexID(ImGui_ImplFoundation_EncodeImTextureID(handle));
        tex->BackendUserData = gTexturePool.get();
    }
    // Actually updating the texture
    // Usually - this is only for updating the (now dynamic) font atlas.
    // TODO: Quite inefficient - not a hot path though. sThis would mostly be about fixing perf issues
    //       with @ref UploadContext - which isn't meant for such cases to begin with ):
    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
    {
        const int upload_x = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.x;
        const int upload_y = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.y;
        const int upload_w = (tex->Status == ImTextureStatus_WantCreate) ? tex->Width : tex->UpdateRect.w;
        const int upload_h = (tex->Status == ImTextureStatus_WantCreate) ? tex->Height : tex->UpdateRect.h;
        size_t upload_pitch = upload_w * tex->BytesPerPixel;
        size_t upload_size = upload_h * upload_pitch;
        auto [hdl, smp] = ImGui_ImplFoundation_DecodeImTextureID(tex->GetTexID());
        RHITexture* texture = gTexturePool->GetTexture(hdl);
        Vector<char> pixels(upload_size, gAllocator.Ptr());
        for (int y = 0; y < upload_h; y++)
            std::memcpy(pixels.data() + upload_pitch * y, tex->GetPixelsAt(upload_x, upload_y + y), upload_pitch);
        auto range = RHITextureSubresourceRange::Create();
        gTextureUploadContext->Upload(texture, pixels, range,
                                      RHICommandList::CopyImageRegion{.dstLayer = range.layer,
                                                                      .dstOffset = {upload_x, upload_y, 0},
                                                                      .extent = {upload_w, upload_h, 1}});
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
    CHECK_MSG(gTexturePool, "Backend not initialized. Did you call ImGui_ImplFoundation_Init()?");
    TexturePoolHandle handle = gTexturePool->Allocate(textureView);
    return ImGui_ImplFoundation_EncodeImTextureID(handle, sampler);
}
void ImGui_ImplFoundation_RemoveImage(ImTextureID textureID)
{
    CHECK_MSG(gTexturePool, "Backend not initialized. Did you call ImGui_ImplFoundation_Init()?");
    auto [hdl, smp] = ImGui_ImplFoundation_DecodeImTextureID(textureID);
    gTexturePool->Free(hdl);
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
                                        ResourceHandle idxBuffer, ResourceHandle linSampler, ResourceHandle nearSampler)
{
    r->BindBackbufferRTV(self, RHIPipelineState::PipelineStateDesc::Attachment::Blending::GetAlphaBlending());
    r->BindBufferCopyDst(self, vtxBuffer);
    r->BindBufferCopyDst(self, idxBuffer);
    r->BindPushConstant(self, RHIShaderStageBits::Vertex, 0, sizeof(PushConstants));
    r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/ImGui.spv");
    r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/ImGui.spv");
    r->BindDescriptorSet(self, "textures", gTexturePool->GetDescriptorSet(), gTexturePool->GetDescriptorSetLayout());
    // We have fixed samplers for ImGui - IMO these two are quite enough for UI elements
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
    // vvv No-op if no creation/updates are required
    gTextureUploadContext->SubmitAndWait();
    // Upload vertex/index buffers
    auto* vtx = r->DerefResource(vtxBuffer).Get<RHIBuffer*>();
    auto* idx = r->DerefResource(idxBuffer).Get<RHIBuffer*>();
    auto pVtx = vtx->MapSpan<ImDrawVert>();
    auto pIdx = idx->MapSpan<ImDrawIdx>();
    ImDrawVert* vtx_dst = pVtx.data();
    ImDrawIdx* idx_dst = pIdx.data();
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        std::memcpy(vtx_dst, draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
        std::memcpy(idx_dst, draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
        vtx_dst += draw_list->VtxBuffer.Size;
        idx_dst += draw_list->IdxBuffer.Size;
    }
    vtx->Flush(), idx->Flush();
    // Implementations guarantee that mapped, flushed resources are available at
    // the time of the next device queue submit - so extra barriers are not needed.
    r->CmdSetPipeline(self, cmd);
    Optional<RHIClearColor> clearColor;
    if (clear)
        clearColor = RHIClearColor{};
    r->CmdBeginGraphics(self, cmd, img_wh, clearColor);
    // Setup states
    int fb_width = img_wh.x, fb_height = img_wh.y;
    cmd->SetViewport(0, 0, fb_width, fb_height); // Full screen
    cmd->BindVertexBuffer(0, {{vtx}}, {{0}});
    cmd->BindIndexBuffer(idx, 0, RHIResourceFormat::R16Uint);
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
            // Bad texture?
            if (!gTexturePool->Contains(textureId))
            {
                pc.textureId = 0; // Reserved for invalid textures
                pc.samplerId = 1; // Nearest neighbor
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
                                              ResourceHandle& outIdxBuffer, ResourceHandle& outLinearSampler,
                                              ResourceHandle& outNearestSampler)
{
    outVtxBuffer = createResource(
        renderer, "ImGui Vertex Buffer",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly},
                      .usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = kVertexBufferSize});
    outIdxBuffer = createResource(
        renderer, "ImGui Index Buffer",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly},
                      .usage = RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = kIndexBufferSize});
    outLinearSampler = createSampler(renderer, {});
    outNearestSampler =
        createSampler(renderer,
                      {.filter = {.minFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor,
                                  .magFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor}});
}
void ImGui_ImplFoundation_SetupContextWithDefaultStyles()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // Enable Docking
    io.ConfigDpiScaleFonts = true; // [Experimental] Automatically overwrite style.FontScaleDpi in Begin() when Monitor
                                   // DPI changes. This will scale fonts but _NOT_ scale sizes/padding for now.
    // Styles from
    // https://github.com/KhronosGroup/Vulkan-Samples/blob/b9961792604af2ede4c9d0868947de2a8eccd549/framework/gui.h#L338
    ImGuiStyle& style = ImGui::GetStyle();
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.005f, 0.005f, 0.005f, 0.94f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.6f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);
    style.Colors[ImGuiCol_MenuBarBg] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_Header] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.8f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.1f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(1.0f, 1.0f, 1.0f, 0.2f);
    style.Colors[ImGuiCol_Button] = ImVec4(1.0f, 0.0f, 0.0f, 0.4f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 0.0f, 0.0f, 0.6f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);
    // Font from https://github.com/lxgw/LxgwNeoXiHei
    if (!std::filesystem::exists(kDefaultFontPath))
        LOG_RUNTIME(ImGui, err, "Font file {} not found! ImGui will use default font.", kDefaultFontPath);
    else
    {
        io.Fonts->Clear();
        ImFontConfig font_cfg;
        io.Fonts->AddFontFromFileTTF(kDefaultFontPath.string().c_str(), 16.0f, &font_cfg);
    }
}
