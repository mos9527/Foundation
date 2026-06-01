#include <cmath>
#include <cfloat>
#include <algorithm>
#include <nfd.h>
#include <Math/Decompose.hpp>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include "EditorState.hpp"
#include "Scene/Mesh.hpp"

static void DrawLightGizmos();

struct PTSPPOption
{
    const char* label;
    uint32_t samplesPerPixel;
    uint32_t dispatchTileSide;
};

static constexpr PTSPPOption kPTSPPOptions[] = {
    {"1/64", 1u, 8u},
    {"1/49", 1u, 7u},
    {"1/36", 1u, 6u},
    {"1/25", 1u, 5u},
    {"1/16", 1u, 4u},
    {"1/9",  1u, 3u},
    {"1/4",  1u, 2u},
    {"1",    1u, 1u},
    {"2",    2u, 1u},
    {"3",    3u, 1u},
    {"4",    4u, 1u},
    {"5",    5u, 1u},
};
static constexpr int kPTSPPOptionCount = std::size(kPTSPPOptions);
static constexpr const char* kExternalViewLUTLabel = "<external>";

template <typename T>
static size_t DrawMemoryStatsTable(const char* tableId, Vector<T>& stats)
{
    size_t totalBytes = 0;
    for (auto const& stat : stats)
        totalBytes += stat.bytes;

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable(tableId, 3, flags))
    {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Usage",
                                ImGuiTableColumnFlags_DefaultSort |
                                ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableSetupColumn("Ratio", ImGuiTableColumnFlags_PreferSortDescending);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
            sortSpecs && sortSpecs->SpecsCount > 0)
        {
            ImGuiTableColumnSortSpecs const& spec = sortSpecs->Specs[0];
            std::sort(stats.begin(), stats.end(), [&](auto const& lhs, auto const& rhs)
            {
                int cmp = 0;
                if (spec.ColumnIndex == 0)
                    cmp = std::strcmp(lhs.name.c_str(), rhs.name.c_str());
                else
                    cmp = lhs.bytes == rhs.bytes ? 0 : (lhs.bytes < rhs.bytes ? -1 : 1);
                return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
            });
        }

        for (auto const& stat : stats)
        {
            float ratio = totalBytes ? static_cast<float>(stat.bytes) / static_cast<float>(totalBytes) : 0.0f;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(stat.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.1f MB", stat.bytes / static_cast<float>(1 << 20u));
            ImGui::TableNextColumn();
            char overlay[32];
            std::snprintf(overlay, sizeof(overlay), "%.1f%%", ratio * 100.0f);
            ImGui::ProgressBar(ratio, ImVec2(-FLT_MIN, 0.0f), overlay);
        }

        ImGui::EndTable();
    }
    return totalBytes;
}

static float BytesToMiB(size_t bytes)
{
    return bytes / static_cast<float>(1 << 20u);
}

static void AppendMemoryFlag(char* buffer, size_t bufferSize, const char* flag)
{
    size_t len = std::strlen(buffer);
    if (len >= bufferSize - 1)
        return;
    if (len != 0)
    {
        std::snprintf(buffer + len, bufferSize - len, " | ");
        len = std::strlen(buffer);
        if (len >= bufferSize - 1)
            return;
    }
    std::snprintf(buffer + len, bufferSize - len, "%s", flag);
}

static const char* MemoryTypeFlagsText(RHIDeviceMemoryTypeStat const& stat, char (&buffer)[128])
{
    buffer[0] = '\0';
    if (stat.deviceLocal)
        AppendMemoryFlag(buffer, sizeof(buffer), "Device Local");
    if (stat.hostVisible)
        AppendMemoryFlag(buffer, sizeof(buffer), "Host Visible");
    if (stat.hostCoherent)
        AppendMemoryFlag(buffer, sizeof(buffer), "Host Coherent");
    if (stat.hostCached)
        AppendMemoryFlag(buffer, sizeof(buffer), "Host Cached");
    if (stat.lazilyAllocated)
        AppendMemoryFlag(buffer, sizeof(buffer), "Lazy");
    if (stat.protectedMemory)
        AppendMemoryFlag(buffer, sizeof(buffer), "Protected");
    if (buffer[0] == '\0')
        std::snprintf(buffer, sizeof(buffer), "None");
    return buffer;
}

static void DrawRHIDeviceHeapStatsTable(RHIDeviceMemoryStats const& stats)
{
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##RHIDeviceHeapStatsTable", 8, flags))
        return;

    ImGui::TableSetupColumn("Heap");
    ImGui::TableSetupColumn("Flags");
    ImGui::TableSetupColumn("Size");
    ImGui::TableSetupColumn("Used/Avail");
    ImGui::TableSetupColumn("VAlloc");
    ImGui::TableSetupColumn("VBlocks");
    ImGui::TableSetupColumn("Slack");
    ImGui::TableSetupColumn("Allocs");
    ImGui::TableHeadersRow();

    for (auto const& heap : stats.heaps)
    {
        size_t slack = heap.blockBytes >= heap.allocationBytes ? heap.blockBytes - heap.allocationBytes : 0;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%u", heap.heapIndex);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(heap.deviceLocal ? "Device Local" : "Host/System");
        ImGui::TableNextColumn();
        ImGui::Text("%.1f MB", BytesToMiB(heap.heapSize));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f / %.1f MB", BytesToMiB(heap.usage), BytesToMiB(heap.budget));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f MB", BytesToMiB(heap.allocationBytes));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f MB", BytesToMiB(heap.blockBytes));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f MB", BytesToMiB(slack));
        ImGui::TableNextColumn();
        ImGui::Text("%u", heap.allocationCount);
    }

    ImGui::EndTable();
}

static void DrawRHIDeviceMemoryTypeStatsTable(RHIDeviceMemoryStats const& stats)
{
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##RHIDeviceMemoryTypeStatsTable", 10, flags))
        return;

    ImGui::TableSetupColumn("Type");
    ImGui::TableSetupColumn("Heap");
    ImGui::TableSetupColumn("Flags");
    ImGui::TableSetupColumn("VAlloc");
    ImGui::TableSetupColumn("VBlocks");
    ImGui::TableSetupColumn("Slack");
    ImGui::TableSetupColumn("Allocs");
    ImGui::TableSetupColumn("Blocks");
    ImGui::TableSetupColumn("Free");
    ImGui::TableSetupColumn("Max");
    ImGui::TableHeadersRow();

    for (auto const& type : stats.memoryTypes)
    {
        size_t slack = type.blockBytes >= type.allocationBytes ? type.blockBytes - type.allocationBytes : 0;
        char flagText[128];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%u", type.typeIndex);
        ImGui::TableNextColumn();
        ImGui::Text("%u", type.heapIndex);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(MemoryTypeFlagsText(type, flagText));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f MB", BytesToMiB(type.allocationBytes));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f MB", BytesToMiB(type.blockBytes));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f MB", BytesToMiB(slack));
        ImGui::TableNextColumn();
        ImGui::Text("%u", type.allocationCount);
        ImGui::TableNextColumn();
        ImGui::Text("%u", type.blockCount);
        ImGui::TableNextColumn();
        ImGui::Text("%u", type.unusedRangeCount);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f MB", BytesToMiB(type.allocationSizeMax));
    }

    ImGui::EndTable();
}

static bool OpenViewLUTDialog(String& outPath)
{
    nfdu8filteritem_t filters[] = {{"DDS LUT", "dds"}};
    nfdopendialogu8args_t args = {0};
    args.filterList = filters;
    args.filterCount = 1;

    nfdu8char_t* selectedPath = nullptr;
    if (NFD_OpenDialogU8_With(&selectedPath, &args) != NFD_OKAY)
        return false;

    outPath = selectedPath;
    NFD_FreePathU8(selectedPath);
    return true;
}

static int PTSPPOptionIndex(UBO const& ubo)
{
    uint32_t samplesPerPixel = PTSamplesPerDispatch(ubo);
    uint32_t dispatchTileSide = PTDispatchTileSide(ubo);
    for (int i = 0; i < kPTSPPOptionCount; ++i)
        if (kPTSPPOptions[i].samplesPerPixel == samplesPerPixel &&
            kPTSPPOptions[i].dispatchTileSide == dispatchTileSide)
            return i;
    return 3; // 1/25 SPP
}

static void SetPTSPPOption(int index)
{
    PTSPPOption const& option = kPTSPPOptions[std::clamp(index, 0, kPTSPPOptionCount - 1)];
    if (GEditor.shaderGlobals.ptSamplesPerPixel != option.samplesPerPixel ||
        GEditor.shaderGlobals.ptDispatchTileSide != option.dispatchTileSide)
    {
        GEditor.shaderGlobals.ptSamplesPerPixel = option.samplesPerPixel;
        GEditor.shaderGlobals.ptDispatchTileSide = option.dispatchTileSide;
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
    }
}

static void DrawAperturePreview(uint32_t blades, float rotation, float ratio)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float size = avail.x < 160.0f ? avail.x : 160.0f;
    size = size < 80.0f ? 80.0f : size;

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("AperturePreview", ImVec2(size, size));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 center = ImVec2(p0.x + size * 0.5f, p0.y + size * 0.5f);
    ImU32 bg = ImGui::GetColorU32(ImVec4(0.12f, 0.12f, 0.12f, 0.55f));
    ImU32 fill = ImGui::GetColorU32(ImVec4(0.72f, 0.72f, 0.72f, 0.55f));
    ImU32 outline = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.90f));

    drawList->AddRectFilled(p0, ImVec2(p0.x + size, p0.y + size), bg, 4.0f);
    drawList->AddRect(p0, ImVec2(p0.x + size, p0.y + size), outline, 4.0f);

    ImVec2 points[64];
    int pointCount = blades >= 3u ? static_cast<int>(blades) : 64;
    pointCount = pointCount > 64 ? 64 : pointCount;

    float ratioSafe = ratio > 1e-3f ? ratio : 1e-3f;
    float xScale = 1.0f / ratioSafe;
    float fitScale = size * 0.42f / (xScale > 1.0f ? xScale : 1.0f);

    for (int i = 0; i < pointCount; ++i)
    {
        float theta = rotation + (2.0f * pi<float>() * float(i)) / float(pointCount);
        points[i] = ImVec2(center.x + std::cos(theta) * xScale * fitScale,
                           center.y + std::sin(theta) * fitScale);
    }

    drawList->AddConvexPolyFilled(points, pointCount, fill);
    drawList->AddPolyline(points, pointCount, outline, ImDrawFlags_Closed, 2.0f);
}

static bool IsSelectedInstanceValid()
{
    return GEditor.HasScene() && GContext->gpuScene &&
           GEditor.selectedInstance >= 0 &&
           GEditor.selectedInstance < static_cast<int>(GContext->gpuScene->GetInstanceCount()) &&
           GEditor.selectedInstance < static_cast<int>(GEditor.Scene().GetInstances().size());
}

static FInstance& SelectedSceneInstance()
{
    return GEditor.Scene().GetInstances()[GEditor.selectedInstance];
}

static const char* InstanceTypeName(uint32_t type)
{
    return type == kGSInstanceTypeCurve ? "Curve" : "Mesh";
}

static int GetSelectedMaterialIndex()
{
    if (IsSelectedInstanceValid() && GContext->gpuScene)
        return static_cast<int>(GContext->gpuScene->GetInstance(
            static_cast<uint32_t>(GEditor.selectedInstance)).materialIndex);
    return GEditor.selectedMaterial;
}

static bool IsMaterialIndexValid(int materialIndex)
{
    return GEditor.HasScene() && GContext->gpuScene &&
           materialIndex >= 0 &&
           materialIndex < static_cast<int>(GContext->gpuScene->GetMaterialCount()) &&
           materialIndex < static_cast<int>(GEditor.Scene().GetMaterials().size());
}

struct TexturePreviewEntry
{
    GPUScene* gpuScene{};
    Renderer* renderer{};
    uint32_t textureIndex{UINT32_MAX};
    ResourceHandle viewHandle{kInvalidHandle};
    ImGui_ImplFoundation_ImageSampler sampler{ImGuiImplFoundationImageSamplerLinear};
    RHITextureView* view{};
    ImTextureID textureID{};
    uint64_t lastSeenFrame{};
};

struct TexturePreviewImage
{
    ImTextureID textureID{};
    RHITextureView* view{};
    uint32_t width{};
    uint32_t height{};
};

enum class TexturePreviewSource
{
    None,
    GPUSceneTexture,
    RendererTextureView,
};

struct TexturePreviewModalState
{
    bool pendingOpen{};
    char title[128]{};
    TexturePreviewSource source{TexturePreviewSource::None};
    GPUScene* gpuScene{};
    Renderer* renderer{};
    uint32_t textureIndex{UINT32_MAX};
    ResourceHandle viewHandle{kInvalidHandle};
    ImGui_ImplFoundation_ImageSampler sampler{ImGuiImplFoundationImageSamplerLinear};
    float zoom{1.0f};
    ImVec2 pan{};
};

static constexpr const char* kTexturePreviewModalName = "Texture Preview";

static uint64_t& TexturePreviewFrame()
{
    static uint64_t frame = 0;
    return frame;
}

static Vector<TexturePreviewEntry>& TexturePreviewCache()
{
    static Vector<TexturePreviewEntry> cache(GLOBAL_ALLOC);
    return cache;
}

static TexturePreviewModalState& TexturePreviewModal()
{
    static TexturePreviewModalState state;
    return state;
}

static void ResetTexturePreviewModal()
{
    auto& state = TexturePreviewModal();
    state.pendingOpen = false;
    state.title[0] = '\0';
    state.source = TexturePreviewSource::None;
    state.gpuScene = nullptr;
    state.renderer = nullptr;
    state.textureIndex = UINT32_MAX;
    state.viewHandle = kInvalidHandle;
    state.sampler = ImGuiImplFoundationImageSamplerLinear;
    state.zoom = 1.0f;
    state.pan = {};
}

void ClearMaterialTexturePreviewCache()
{
    auto& cache = TexturePreviewCache();
    for (auto& entry : cache)
        if (entry.textureID != 0)
            ImGui_ImplFoundation_RemoveImage(entry.textureID);
    cache.clear();
    ResetTexturePreviewModal();
}

static TexturePreviewImage GetTexturePreviewImage(uint32_t textureIndex, ImGui_ImplFoundation_ImageSampler sampler)
{
    if (!GContext || !GContext->gpuScene || textureIndex == UINT32_MAX)
        return {};

    auto* pool = GContext->gpuScene->GetTexture2DPool();
    RHITextureView* view = pool ? pool->GetView(textureIndex) : nullptr;
    RHITexture* texture = view ? view->GetTexture() : nullptr;
    if (!texture)
        return {};

    uint32_t width = texture->mDesc.extent.x;
    uint32_t height = texture->mDesc.extent.y;
    auto& cache = TexturePreviewCache();
    for (auto& entry : cache)
    {
        if (entry.gpuScene != GContext->gpuScene || entry.renderer != nullptr ||
            entry.textureIndex != textureIndex || entry.sampler != sampler)
            continue;

        if (entry.view == view)
            return {entry.textureID, view, width, height};

        if (entry.textureID != 0)
            ImGui_ImplFoundation_RemoveImage(entry.textureID);
        entry.view = view;
        entry.textureID = ImGui_ImplFoundation_AddImage(view, sampler);
        return {entry.textureID, view, width, height};
    }

    ImTextureID textureID = ImGui_ImplFoundation_AddImage(view, sampler);
    cache.push_back(TexturePreviewEntry{
        .gpuScene = GContext->gpuScene,
        .textureIndex = textureIndex,
        .sampler = sampler,
        .view = view,
        .textureID = textureID,
    });
    return {textureID, view, width, height};
}

static TexturePreviewImage GetTexturePreviewImage(Renderer* renderer, ResourceHandle viewHandle, RHITextureView* view,
                                                  ImGui_ImplFoundation_ImageSampler sampler)
{
    if (!renderer || viewHandle == kInvalidHandle || !view)
        return {};

    RHITexture* texture = view->GetTexture();
    if (!texture)
        return {};

    uint32_t width = texture->mDesc.extent.x;
    uint32_t height = texture->mDesc.extent.y;
    uint64_t frame = TexturePreviewFrame();
    auto& cache = TexturePreviewCache();
    for (auto& entry : cache)
    {
        if (entry.renderer != renderer || entry.viewHandle != viewHandle || entry.sampler != sampler)
            continue;

        entry.lastSeenFrame = frame;
        if (entry.view == view)
            return {entry.textureID, view, width, height};

        if (entry.textureID != 0)
            ImGui_ImplFoundation_RemoveImage(entry.textureID);
        entry.view = view;
        entry.textureID = ImGui_ImplFoundation_AddImage(view, sampler);
        return {entry.textureID, view, width, height};
    }

    ImTextureID textureID = ImGui_ImplFoundation_AddImage(view, sampler);
    cache.push_back(TexturePreviewEntry{
        .renderer = renderer,
        .viewHandle = viewHandle,
        .sampler = sampler,
        .view = view,
        .textureID = textureID,
        .lastSeenFrame = frame,
    });
    return {textureID, view, width, height};
}

static void PruneTexturePreviewCache(uint64_t frame)
{
    auto& cache = TexturePreviewCache();
    for (auto it = cache.begin(); it != cache.end();)
    {
        bool prune = it->renderer != nullptr && it->lastSeenFrame + 1 < frame;
        if (!prune)
        {
            ++it;
            continue;
        }

        if (it->textureID != 0)
            ImGui_ImplFoundation_RemoveImage(it->textureID);
        it = cache.erase(it);
    }
}

static bool IsTexturePreviewFormatSupported(RHIResourceFormat format);

static TexturePreviewImage ResolveTexturePreviewModalImage(TexturePreviewModalState const& state)
{
    switch (state.source)
    {
    case TexturePreviewSource::GPUSceneTexture:
        if (!GContext || state.gpuScene != GContext->gpuScene || state.textureIndex == UINT32_MAX)
            return {};
        return GetTexturePreviewImage(state.textureIndex, state.sampler);
    case TexturePreviewSource::RendererTextureView:
    {
        if (!GContext || state.renderer != GContext->renderer || state.viewHandle == kInvalidHandle)
            return {};

        Vector<Renderer::TexturePreviewStat> previews(GLOBAL_ALLOC);
        state.renderer->DbgGetTexturePreviews(previews);
        auto it = Ranges::find_if(previews,
                                  [&state](auto const& item) { return item.viewHandle == state.viewHandle; });
        if (it == previews.end() || !it->view || !IsTexturePreviewFormatSupported(it->format))
            return {};

        return GetTexturePreviewImage(state.renderer, state.viewHandle, it->view, state.sampler);
    }
    case TexturePreviewSource::None:
    default:
        return {};
    }
}

static void OpenTexturePreviewModal(const char* title, GPUScene* gpuScene, uint32_t textureIndex,
                                    ImGui_ImplFoundation_ImageSampler sampler, TexturePreviewImage const& preview)
{
    if (preview.textureID == 0 || preview.width == 0 || preview.height == 0)
        return;

    auto& state = TexturePreviewModal();
    state.pendingOpen = true;
    std::snprintf(state.title, sizeof(state.title), "%s", title);
    state.source = TexturePreviewSource::GPUSceneTexture;
    state.gpuScene = gpuScene;
    state.renderer = nullptr;
    state.textureIndex = textureIndex;
    state.viewHandle = kInvalidHandle;
    state.sampler = sampler;
    state.zoom = 1.0f;
    state.pan = {};
}

static void OpenTexturePreviewModal(const char* label, uint32_t textureIndex,
                                    ImGui_ImplFoundation_ImageSampler sampler, TexturePreviewImage const& preview)
{
    char title[128];
    std::snprintf(title, sizeof(title), "%s #%u", label, textureIndex);
    OpenTexturePreviewModal(title, GContext ? GContext->gpuScene : nullptr, textureIndex, sampler, preview);
}

static void OpenTexturePreviewModal(const char* title, Renderer* renderer, ResourceHandle viewHandle,
                                    ImGui_ImplFoundation_ImageSampler sampler, TexturePreviewImage const& preview)
{
    if (preview.textureID == 0 || preview.width == 0 || preview.height == 0)
        return;

    auto& state = TexturePreviewModal();
    state.pendingOpen = true;
    std::snprintf(state.title, sizeof(state.title), "%s", title);
    state.source = TexturePreviewSource::RendererTextureView;
    state.gpuScene = nullptr;
    state.renderer = renderer;
    state.textureIndex = UINT32_MAX;
    state.viewHandle = viewHandle;
    state.sampler = sampler;
    state.zoom = 1.0f;
    state.pan = {};
}

static void DrawTexturePreviewModal()
{
    auto& state = TexturePreviewModal();
    if (state.pendingOpen)
    {
        ImGui::OpenPopup(kTexturePreviewModalName);
        state.pendingOpen = false;
    }

    ImGui::SetNextWindowSize(ImVec2(760.0f, 560.0f), ImGuiCond_FirstUseEver);
    bool modalOpen = true;
    if (!ImGui::BeginPopupModal(kTexturePreviewModalName, &modalOpen,
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        return;

    if (!modalOpen)
    {
        ResetTexturePreviewModal();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    TexturePreviewImage preview = ResolveTexturePreviewModalImage(state);
    if (preview.textureID == 0 || preview.width == 0 || preview.height == 0)
    {
        ImGui::TextUnformatted("Texture is unavailable.");
        if (ImGui::Button("Close"))
        {
            ResetTexturePreviewModal();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    float textureWidth = static_cast<float>(std::max(1u, preview.width));
    float textureHeight = static_cast<float>(std::max(1u, preview.height));
    ImGui::Text("%s  (%.0fx%.0f)", state.title, textureWidth, textureHeight);
    ImGui::SameLine();
    ImGui::TextDisabled("Mouse wheel: zoom at cursor, drag: pan");

    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Zoom", &state.zoom, 0.05f, 16.0f, "%.2fx");
    ImGui::SameLine();
    if (ImGui::Button("Reset"))
    {
        state.zoom = 1.0f;
        state.pan = {};
    }
    ImGui::SameLine();
    if (ImGui::Button("Close"))
    {
        ResetTexturePreviewModal();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(canvasSize.x, 1.0f);
    canvasSize.y = std::max(canvasSize.y, 1.0f);
    ImGui::BeginChild("TexturePreviewCanvas", canvasSize, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 innerSize = ImGui::GetContentRegionAvail();
    innerSize.x = std::max(innerSize.x, 1.0f);
    innerSize.y = std::max(innerSize.y, 1.0f);
    ImGui::InvisibleButton("TexturePreviewDragSurface", innerSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);

    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 canvasCenter{canvasPos.x + innerSize.x * 0.5f, canvasPos.y + innerSize.y * 0.5f};
    if (hovered && io.MouseWheel != 0.0f)
    {
        float oldZoom = state.zoom;
        float newZoom = std::clamp(oldZoom * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f), 0.05f, 16.0f);
        if (newZoom != oldZoom)
        {
            float zoomRatio = newZoom / oldZoom;
            ImVec2 mouseFromImageCenter{io.MousePos.x - canvasCenter.x - state.pan.x,
                                        io.MousePos.y - canvasCenter.y - state.pan.y};
            state.pan.x += mouseFromImageCenter.x * (1.0f - zoomRatio);
            state.pan.y += mouseFromImageCenter.y * (1.0f - zoomRatio);
            state.zoom = newZoom;
        }
    }
    if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
    {
        state.pan.x += io.MouseDelta.x;
        state.pan.y += io.MouseDelta.y;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasMax{canvasPos.x + innerSize.x, canvasPos.y + innerSize.y};
    drawList->AddRectFilled(canvasPos, canvasMax, IM_COL32(18, 18, 18, 255));

    float fitScale = std::min(innerSize.x / textureWidth, innerSize.y / textureHeight);
    fitScale = std::max(fitScale, 0.001f);
    ImVec2 imageSize{textureWidth * fitScale * state.zoom, textureHeight * fitScale * state.zoom};
    ImVec2 center{canvasCenter.x + state.pan.x, canvasCenter.y + state.pan.y};
    ImVec2 imageMin{center.x - imageSize.x * 0.5f, center.y - imageSize.y * 0.5f};
    ImVec2 imageMax{center.x + imageSize.x * 0.5f, center.y + imageSize.y * 0.5f};
    drawList->AddImage(preview.textureID, imageMin, imageMax);
    drawList->AddRect(imageMin, imageMax, IM_COL32(255, 255, 255, 64));

    ImGui::EndChild();
    ImGui::EndPopup();
}

static void DrawTexturePreview(const char* label, uint32_t textureIndex,
                               ImGui_ImplFoundation_ImageSampler sampler = ImGuiImplFoundationImageSamplerLinear)
{
    ImGui::PushID(label);
    ImVec2 previewSize{48.0f, 48.0f};
    TexturePreviewImage preview = GetTexturePreviewImage(textureIndex, sampler);
    if (preview.textureID != 0)
    {
        if (ImGui::ImageButton("Preview", preview.textureID, previewSize))
            OpenTexturePreviewModal(label, textureIndex, sampler, preview);
    }
    else
    {
        ImGui::BeginDisabled();
        ImGui::Button("None", previewSize);
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (textureIndex == UINT32_MAX)
        ImGui::Text("%s: none", label);
    else if (preview.textureID == 0)
        ImGui::Text("%s: %u (unavailable)", label, textureIndex);
    else
        ImGui::Text("%s: %u", label, textureIndex);
    ImGui::PopID();
}

static bool IsTexturePreviewFormatSupported(RHIResourceFormat format)
{
    switch (format)
    {
    case RHIResourceFormat::R8G8B8A8Unorm:
    case RHIResourceFormat::R8G8B8A8Srgb:
    case RHIResourceFormat::B8G8R8A8Unrom:
    case RHIResourceFormat::B8G8R8A8Srgb:
    case RHIResourceFormat::A2R10G10B10Unorm:
    case RHIResourceFormat::A2B10G10R10Unorm:
    case RHIResourceFormat::B10G11R11Ufloat:
    case RHIResourceFormat::R32SignedFloat:
    case RHIResourceFormat::R32G32SignedFloat:
    case RHIResourceFormat::R32G32B32SignedFloat:
    case RHIResourceFormat::R32G32B32A32SignedFloat:
    case RHIResourceFormat::R16SignedFloat:
    case RHIResourceFormat::R16G16SignedFloat:
    case RHIResourceFormat::R16G16B16SignedFloat:
    case RHIResourceFormat::R16G16B16A16SignedFloat:
    case RHIResourceFormat::R16Unorm:
    case RHIResourceFormat::Bc1RgbUnorm:
    case RHIResourceFormat::Bc1RgbSrgb:
    case RHIResourceFormat::Bc1RgbaUnorm:
    case RHIResourceFormat::Bc1RgbaSrgb:
    case RHIResourceFormat::Bc2Unorm:
    case RHIResourceFormat::Bc2Srgb:
    case RHIResourceFormat::Bc3Unorm:
    case RHIResourceFormat::Bc3Srgb:
    case RHIResourceFormat::Bc4Unorm:
    case RHIResourceFormat::Bc4Snorm:
    case RHIResourceFormat::Bc5Unorm:
    case RHIResourceFormat::Bc5Snorm:
    case RHIResourceFormat::Bc6HUfloat:
    case RHIResourceFormat::Bc6HSfloat:
    case RHIResourceFormat::Bc7Unorm:
    case RHIResourceFormat::Bc7Srgb:
        return true;
    default:
        return false;
    }
}

static void DrawRenderGraphTexturePreviews(Renderer* renderer, Allocator* scratch)
{
    Vector<Renderer::TexturePreviewStat> previews(scratch);
    renderer->DbgGetTexturePreviews(previews);
    ImGui::SeparatorText("Texture Views");
    if (previews.empty())
    {
        ImGui::TextDisabled("No texture views available");
        return;
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##RenderGraphTexturePreviewTable", 5, flags))
        return;

    ImGui::TableSetupColumn("Preview", ImGuiTableColumnFlags_WidthFixed, 58.0f);
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("Resource", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("View", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 96.0f);
    ImGui::TableHeadersRow();

    ImVec2 previewSize{48.0f, 48.0f};
    for (auto const& item : previews)
    {
        RHITexture* texture = item.view ? item.view->GetTexture() : nullptr;
        bool supported = texture && IsTexturePreviewFormatSupported(item.format);
        TexturePreviewImage preview = supported ? GetTexturePreviewImage(renderer, item.viewHandle, item.view,
                                                                         ImGuiImplFoundationImageSamplerLinear)
                                               : TexturePreviewImage{};
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char previewId[32];
        std::snprintf(previewId, sizeof(previewId), "Preview##%zu", static_cast<size_t>(item.viewHandle));
        if (preview.textureID != 0)
        {
            if (ImGui::ImageButton(previewId, preview.textureID, previewSize))
            {
                char title[128];
                std::snprintf(title, sizeof(title), "RenderGraph %s view #%zu", item.name.c_str(),
                              static_cast<size_t>(item.viewHandle));
                OpenTexturePreviewModal(title, renderer, item.viewHandle, ImGuiImplFoundationImageSamplerLinear,
                                        preview);
            }
        }
        else
        {
            ImGui::BeginDisabled();
            ImGui::Button(supported ? "N/A" : "Skip", previewSize);
            ImGui::EndDisabled();
        }

        ImGui::TableNextColumn();
        ImGui::TextUnformatted(item.name.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%zu", static_cast<size_t>(item.resourceHandle));
        ImGui::TableNextColumn();
        ImGui::Text("%zu", static_cast<size_t>(item.viewHandle));
        ImGui::TableNextColumn();
        if (texture)
            ImGui::Text("%ux%u", texture->mDesc.extent.x, texture->mDesc.extent.y);
        else
            ImGui::TextUnformatted("unavailable");
    }

    ImGui::EndTable();
}

void EditorDockSpaceAndMenuBar()
{
    // Semi-transparent window background
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));

    // DockSpace covers the full viewport; transparent background to show the backbuffer.
    ImGuiID dockspaceID = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

    // Set up default layout on first launch
    static bool firstTime = true;
    if (firstTime)
    {
        firstTime = false;
        ImGui::DockBuilderRemoveNode(dockspaceID);
        ImGui::DockBuilderAddNode(dockspaceID, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceID, ImGui::GetMainViewport()->Size);

        ImGuiID dockLeft, dockCenter;
        ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Left, 0.20f, &dockLeft, &dockCenter);
        ImGuiID dockRight;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.25f, &dockRight, &dockCenter);

        ImGuiID dockLeftTop, dockLeftBottom;
        ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Up, 0.5f, &dockLeftTop, &dockLeftBottom);
        ImGui::DockBuilderDockWindow("Hierarchy", dockLeftTop);
        ImGui::DockBuilderDockWindow("Inspector", dockLeftBottom);
        ImGui::DockBuilderDockWindow("Material", dockLeftBottom);
        ImGui::DockBuilderDockWindow("Camera", dockRight);
        ImGui::DockBuilderDockWindow("Lighting", dockRight);
        ImGui::DockBuilderDockWindow("Rendering", dockRight);
        ImGui::DockBuilderDockWindow("Profiler", dockRight);
        ImGui::DockBuilderFinish(dockspaceID);
    }

    // Main menu bar
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Open Scene..."))
            {
                nfdu8filteritem_t filters[] = {{"Scene Files", "gltf,glb,fscn"}};
                nfdopendialogu8args_t args = {0};
                args.filterList = filters;
                args.filterCount = 1;
                nfdu8char_t* outPath = nullptr;
                if (NFD_OpenDialogU8_With(&outPath, &args) == NFD_OKAY)
                {
                    LoadScene(outPath);
                    NFD_FreePathU8(outPath);
                }
            }
            if (ImGui::MenuItem("Open HDR..."))
            {
                nfdu8filteritem_t filters[] = {{"HDR Images", "hdr,hdri"}};
                nfdopendialogu8args_t args = {0};
                args.filterList = filters;
                args.filterCount = 1;
                nfdu8char_t* outPath = nullptr;
                if (NFD_OpenDialogU8_With(&outPath, &args) == NFD_OKAY)
                {
                    LoadEnvMap(outPath);
                    NFD_FreePathU8(outPath);
                }
            }
            ImGui::Separator();
            if (GContext->gpuScene && GContext->gpuScene->GetInstanceCount() != 0)
            {
                if (ImGui::MenuItem("Render HDR..."))
                {
                    nfdu8filteritem_t filters[] = {{"Radiance HDR", "hdr"}};
                    nfdsavedialogu8args_t args = {0};
                    args.filterList = filters;
                    args.filterCount = 1;
                    nfdu8char_t* outPath = nullptr;
                    if (NFD_SaveDialogU8_With(&outPath, &args) == NFD_OKAY)
                    {
                        GEditor.renderTask.outputPath = outPath;
                        GEditor.renderTask.format = ERenderFormat::HDR;
                        GEditor.renderTask.openRenderPopup = true;
                        NFD_FreePathU8(outPath);
                    }
                }
                if (ImGui::MenuItem("Render SDR...", nullptr, false, !GContext->enableHDR))
                {
                    nfdu8filteritem_t filters[] = {{"PNG Image", "png"}};
                    nfdsavedialogu8args_t args = {0};
                    args.filterList = filters;
                    args.filterCount = 1;
                    nfdu8char_t* outPath = nullptr;
                    if (NFD_SaveDialogU8_With(&outPath, &args) == NFD_OKAY)
                    {
                        GEditor.renderTask.outputPath = outPath;
                        GEditor.renderTask.format = ERenderFormat::SDR;
                        GEditor.renderTask.openRenderPopup = true;
                        NFD_FreePathU8(outPath);
                    }
                }
                if (GContext->enableHDR && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Disable HDR output before rendering SDR PNGs.");
            }
            ImGui::EndMenu();
        }

        // Render Settings modal popup (opened after file dialog)
        if (GEditor.renderTask.openRenderPopup)
        {
            ImGui::OpenPopup("Render Settings");
            GEditor.renderTask.openRenderPopup = false;
        }
        if (ImGui::BeginPopupModal("Render Settings", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            const bool pathTracerRender = GEditor.rendererMode == ERendererMode::PathTracer;
            const char* formatLabel = GEditor.renderTask.format == ERenderFormat::HDR ? "HDR" : "SDR";
            ImGui::Text("Configure %s %s render:", pathTracerRender ? "path tracer" : "raster", formatLabel);
            ImGui::Text("Output: %s", GEditor.renderTask.outputPath.c_str());
            ImGui::Separator();
            if (pathTracerRender)
            {
                ImGui::InputInt("Samples / pixel", &GEditor.renderTask.samplePopupInput);
                if (GEditor.renderTask.samplePopupInput < 1)
                    GEditor.renderTask.samplePopupInput = 1;
            }
            else
            {
                ImGui::TextUnformatted("Raster export captures the next rendered frame.");
            }
            if (ImGui::Button("Start Render"))
            {
                GEditor.renderTask.targetSamples = pathTracerRender ? GEditor.renderTask.samplePopupInput : 1;
                GEditor.renderTask.renderPaused = false;
                GEditor.renderTask.renderAutoPaused = false;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.renderTask.previousSpp = GEditor.shaderGlobals.ptSamplesPerPixel;
                GEditor.renderTask.previousSppTile = GEditor.shaderGlobals.ptDispatchTileSide;
                // Go for 1spp always during rendering
                GEditor.shaderGlobals.ptSamplesPerPixel = 1;
                GEditor.shaderGlobals.ptDispatchTileSide = 1;
                GEditor.state = FERendering;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // Right-aligned PT / Raster toggle
        if (GContext->gpuScene && GContext->gpuScene->GetInstanceCount() != 0)
        {
            float btnW_PT = ImGui::CalcTextSize("######").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float btnW_R = ImGui::CalcTextSize("######").x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float totalW = btnW_PT + btnW_R;
            float avail = ImGui::GetContentRegionAvail().x;
            if (avail > totalW)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - totalW);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            if (GEditor.rendererMode == ERendererMode::PathTracer)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            else
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            // Three pause states drive the PT button label:
            //   Running       -> "  PT  "
            //   ManualPaused  -> blinking "PAUSED" (sticky; only PT click clears)
            //   AutoPaused    -> blinking "AUTO"   (cleared by any user operation)
            const char* labelPTPause[]   = {"  PT  ", "", "PAUSED", ""};
            const char* labelPTAuto[]    = {"  PT  ", "", " AUTO ", ""};
            int blink = (SDL_GetTicks() >> 9) & 3;
            const char* ptLabel = "  PT  ";
            if (GEditor.renderTask.renderAutoPaused)
                ptLabel = labelPTAuto[blink];
            else if (GEditor.renderTask.renderPaused)
                ptLabel = labelPTPause[blink];
            if (ImGui::Button(ptLabel, ImVec2(btnW_PT, 0)))
            {
                if (GEditor.rendererMode != ERendererMode::PathTracer)
                {
                    GEditor.rendererMode = ERendererMode::PathTracer;
                    GEditor.state = FERunningEnter;
                    GEditor.renderTask.renderPaused = false;
                    GEditor.renderTask.renderAutoPaused = false;
                }
                else if (GEditor.renderTask.renderAutoPaused)
                {
                    // Clicking while AutoPaused -> resume rendering. Treat as user op.
                    GEditor.renderTask.renderPaused = false;
                    GEditor.renderTask.renderAutoPaused = false;
                }
                else
                {
                    // Manual toggle. Always sticky - never auto.
                    GEditor.renderTask.renderPaused ^= 1;
                    GEditor.renderTask.renderAutoPaused = false;
                }
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();

            if (GEditor.rendererMode == ERendererMode::Raster)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            else
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Button]);
            if (ImGui::Button("RASTER"))
            {
                if (GEditor.rendererMode != ERendererMode::Raster)
                {
                    GEditor.rendererMode = ERendererMode::Raster;
                    GEditor.state = FERunningEnter;
                }
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        ImGui::EndMainMenuBar();
    }

    ImGui::PopStyleColor(); // WindowBg
}

void FHierarchyPanel()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Hierarchy"))
    {
        if (!GContext->gpuScene || GContext->gpuScene->GetInstanceCount() == 0)
        {
            ImGui::TextDisabled("No instances loaded");
        }
        else
        {
            auto* gpu = GContext->gpuScene;
            uint32_t instanceCount = gpu->GetInstanceCount();
            ImGui::Text("%u instances", instanceCount);
            ImGui::Separator();
            for (uint32_t i = 0; i < instanceCount; i++)
            {
                GSInstance inst = gpu->GetInstance(i);
                char label[128];
                snprintf(label, sizeof(label), "%s %u -- Resource %u, Mat %u",
                         InstanceTypeName(inst.type), i, inst.resourceIndex, inst.materialIndex);
                bool selected = (GEditor.selectedInstance == static_cast<int>(i));
                if (ImGui::Selectable(label, selected))
                {
                    GEditor.selectedInstance = static_cast<int>(i);
                    GEditor.selectedMaterial = static_cast<int>(inst.materialIndex);
                    GEditor.selectedLight = -1; // deselect light when selecting instance
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // Material panel for the selected instance
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Material"))
    {
        int materialIndex = GetSelectedMaterialIndex();
        if (IsMaterialIndexValid(materialIndex))
        {
            GEditor.selectedMaterial = materialIndex;

            auto& material = GEditor.Scene().GetMaterials()[materialIndex];
            GSMaterial gpuMaterial = GContext->gpuScene->GetMaterial(materialIndex);
            ImGui::Text("Material %d", materialIndex);
            if (IsSelectedInstanceValid())
                ImGui::Text("From Instance %d", GEditor.selectedInstance);
            ImGui::Separator();

            bool changed = false;
            const char* shaderBlockLabels[] = {"Principled", "Hair"};
            int shaderBlock = static_cast<int>(material.shaderBlockID);
            if (ImGui::Combo("Shader Block", &shaderBlock, shaderBlockLabels, IM_ARRAYSIZE(shaderBlockLabels)))
            {
                material.shaderBlockID = static_cast<FMaterialShaderBlock>(shaderBlock);
                changed = true;
            }

            ImGui::SeparatorText("Principled");
            changed |= ImGui::ColorEdit4("Base Color", &material.baseColorFactor.x);
            changed |= ImHDRColorEdit("Emissive", reinterpret_cast<float3&>(material.emissiveFactor), material.emissiveFactor.w /* otherwise unused */);
            changed |= ImGui::SliderFloat("Metallic", &material.metallicFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Roughness", &material.roughnessFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Transmission", &material.transmissionFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("IOR", &material.ior, 1.0f, 3.0f, "%.3f");
            changed |= ImGui::SliderFloat("Specular", &material.specularFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::DragFloat3("Specular Color", &material.specularColorFactor.x, 0.01f, 0.0f, FLT_MAX, "%.3f");
            changed |= ImGui::SliderFloat("Anisotropy Strength", &material.anisotropyStrength, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::DragFloat("Anisotropy Rotation", &material.anisotropyRotation, 0.01f, -FLT_MAX, FLT_MAX, "%.3f rad");

            ImGui::SeparatorText("Sheen");
            float sheenWeight = std::clamp(std::max({material.sheenColorFactor.x, material.sheenColorFactor.y, material.sheenColorFactor.z}), 0.0f, 1.0f);
            float3 sheenTint = sheenWeight > 1e-6f ? material.sheenColorFactor / sheenWeight : float3{1.0f, 1.0f, 1.0f};
            bool sheenChanged = false;
            sheenChanged |= ImGui::SliderFloat("Sheen Weight", &sheenWeight, 0.0f, 1.0f, "%.3f");
            sheenChanged |= ImGui::ColorEdit3("Sheen Tint", &sheenTint.x);
            if (sheenChanged)
            {
                material.sheenColorFactor = std::clamp(sheenWeight, 0.0f, 1.0f) * Math::clamp(sheenTint, float3{0.0f, 0.0f, 0.0f}, float3{1.0f, 1.0f, 1.0f});
                changed = true;
            }
            changed |= ImGui::SliderFloat("Sheen Roughness", &material.sheenRoughnessFactor, 0.0f, 1.0f, "%.3f");

            ImGui::SeparatorText("Clearcoat");
            changed |= ImGui::SliderFloat("Clearcoat Weight", &material.clearcoatFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Clearcoat Roughness", &material.clearcoatRoughnessFactor, 0.0f, 1.0f, "%.3f");

            ImGui::SeparatorText("Hair");
            changed |= ImGui::SliderFloat("Beta M", &material.hairBetaM, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::SliderFloat("Beta N", &material.hairBetaN, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::DragFloat("Alpha", &material.hairAlpha, 0.1f, -20.0f, 20.0f, "%.2f deg");

            ImGui::SeparatorText("Subsurface");
            changed |= ImGui::SliderFloat("Weight", &material.subsurfaceFactor, 0.0f, 1.0f, "%.3f");
            changed |= ImGui::ColorEdit3("Color", &material.subsurfaceColor.x);
            changed |= ImGui::DragFloat3("Radius", &material.subsurfaceRadius.x, 0.001f, 0.0f, FLT_MAX, "%.4f");
            changed |= ImGui::SliderFloat("Scale", &material.subsurfaceScale, 0.0f, 1.0f, "%.4f");

            ImGui::SeparatorText("Textures");
            DrawTexturePreview("Base Color", gpuMaterial.baseColorTexture);
            DrawTexturePreview("Emissive", gpuMaterial.emissiveTexture);
            DrawTexturePreview("Metallic/Roughness", gpuMaterial.metallicRoughnessTexture);
            DrawTexturePreview("Normal", gpuMaterial.normalTexture, ImGuiImplFoundationImageSamplerNearest);
            DrawTexturePreview("Transmission", gpuMaterial.transmissionTexture);
            DrawTexturePreview("Specular", gpuMaterial.specularTexture);
            DrawTexturePreview("Specular Color", gpuMaterial.specularColorTexture);
            DrawTexturePreview("Anisotropy", gpuMaterial.anisotropyTexture);
            DrawTexturePreview("Sheen Color", gpuMaterial.sheenColorTexture);
            DrawTexturePreview("Sheen Roughness", gpuMaterial.sheenRoughnessTexture);
            DrawTexturePreview("Clearcoat", gpuMaterial.clearcoatTexture);
            DrawTexturePreview("Clearcoat Roughness", gpuMaterial.clearcoatRoughnessTexture);

            if (changed)
            {
                // Material edits live on the scene; the commit refills the GPU table
                // (with texture remap) from it.
                CommitSceneToGPU(true);
            }
        }
        else if (IsSelectedInstanceValid())
        {
            ImGui::TextDisabled("Selected instance has invalid material index");
        }
        else
        {
            ImGui::TextDisabled("No instance selected");
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // Inspector panel (Instance)
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Inspector"))
    {
        if (IsSelectedInstanceValid())
        {
            auto& pi = SelectedSceneInstance();
            GSInstance inst = GContext->gpuScene->GetInstance(static_cast<uint32_t>(GEditor.selectedInstance));
            ImGui::Text("%s Instance %d", InstanceTypeName(inst.type), GEditor.selectedInstance);
            ImGui::Separator();
            bool changed = false;
            changed |= ImGui::DragFloat3("Position", &pi.transform.transform.x, 0.01f);
            changed |= ImGui::DragFloat4("Rotation", &pi.transform.rotation.x, 0.001f);
            changed |= ImGui::DragFloat3("Scale", &pi.transform.scale.x, 0.01f);

            // -- Gizmo controls --
            ImGui::Separator();
            if (ImGui::RadioButton("Translate (G)", GEditor.gizmo.op == ImGuizmo::TRANSLATE))
                GEditor.gizmo.op = ImGuizmo::TRANSLATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Rotate (R)", GEditor.gizmo.op == ImGuizmo::ROTATE))
                GEditor.gizmo.op = ImGuizmo::ROTATE;
            ImGui::SameLine();
            if (ImGui::RadioButton("Scale (Q)", GEditor.gizmo.op == ImGuizmo::SCALE))
                GEditor.gizmo.op = ImGuizmo::SCALE;
            if (GEditor.gizmo.op != ImGuizmo::SCALE)
            {
                if (ImGui::RadioButton("Local", GEditor.gizmo.mode == ImGuizmo::LOCAL))
                    GEditor.gizmo.mode = ImGuizmo::LOCAL;
                ImGui::SameLine();
                if (ImGui::RadioButton("World", GEditor.gizmo.mode == ImGuizmo::WORLD))
                    GEditor.gizmo.mode = ImGuizmo::WORLD;
            }

            // Build model matrix (TRS -> mat4)
            mat4 modelMatrix = translate(mat4(1.0f), vec3(pi.transform.transform)) * mat4_cast(pi.transform.rotation) *
                glm::scale(mat4(1.0f), vec3(pi.transform.scale));

            // ImGuizmo rendering — only when no light is selected (mutual exclusion)
            if (GEditor.selectedLight < 0 && GEditor.viewport.HasRect())
            {
                ImGuizmo::BeginFrame();
                ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
                ImVec2 viewportSize = GEditor.viewport.Size();
                ImGuizmo::SetRect(GEditor.viewport.contentMin.x, GEditor.viewport.contentMin.y,
                                  viewportSize.x, viewportSize.y);
                // Note: ImGuizmo uses column-major float[16], matching GLM mat4 memory layout
                if (ImGuizmo::Manipulate(&GEditor.camera.view[0][0], &GEditor.camera.proj[0][0], static_cast<ImGuizmo::OPERATION>(GEditor.gizmo.op), static_cast<ImGuizmo::MODE>(GEditor.gizmo.mode),
                                         &modelMatrix[0][0]))
                {
                    // Decompose back to TRS
                    float3 newTranslation;
                    quat newRotation;
                    float3 newScale;
                    Math::decompose(modelMatrix, newScale, newRotation, newTranslation);
                    pi.transform.transform = newTranslation;
                    pi.transform.rotation = newRotation;
                    pi.transform.scale = newScale;
                    changed = true;
                }
            }
            if (changed)
            {
                // The transform edit lives on the scene instance; recommit refills the
                // GPU instance table. Reread the snapshot for the readout below.
                CommitSceneToGPU(true);
                inst = GContext->gpuScene->GetInstance(static_cast<uint32_t>(GEditor.selectedInstance));
            }
            ImGui::Separator();
            ImGui::Text("Type: %s", InstanceTypeName(inst.type));
            ImGui::Text("Resource Offset: %u", inst.resourceOffset);
            ImGui::Text("Material Index: %u", inst.materialIndex);
            ImGui::Text("Resource Index: %u", inst.resourceIndex);
            ImGui::Separator();
            // Invalidates inst/pi references; must be the last use this frame.
            if (ImGui::Button("Delete Instance"))
                DeleteSelectedInstance();
        }
        else
        {
            ImGui::TextDisabled("No instance selected");
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void FLightingPanel()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Lighting"))
    {
        if (!GEditor.HasScene())
        {
            ImGui::TextDisabled("No scene loaded");
            ImGui::End();
            ImGui::PopStyleColor();
            return;
        }

        bool anyChanged = false;
        static const char* kLightTypeNames[] = {"Directional", "Point", "Spot", "Disk", "Rect"};
        static constexpr int kLightTypeCount = 5;

        // ---- Scene Lights ----
        if (ImGui::CollapsingHeader("Scene Lights", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto& lights = GEditor.Scene().mTables.lights;
            uint32_t lightCapacity = GContext->gpuScene ? GContext->gpuScene->GetLightCapacity() : 0u;
            bool canAddLight = GContext->gpuScene && lights.size() < lightCapacity;
            ImGui::TextDisabled("Editor-only light edits update the resident scene and GPU state; the FSCN file is unchanged.");
            if (!canAddLight)
                ImGui::BeginDisabled();
            if (ImGui::Button("Add Test Light"))
            {
                FLight light{};
                light.type = FLightType::Point;
                light.transform.transform = GEditor.camera.center;
                light.transform.rotation = GEditor.camera.rot;
                light.color = float3{1.0f, 0.92f, 0.78f};
                light.power = 10.0f;
                light.range = 10.0f;
                lights.push_back(light);
                GEditor.selectedLight = static_cast<int>(lights.size()) - 1;
                GEditor.selectedInstance = -1;
                GEditor.selectedMaterial = -1;
                anyChanged = true;
            }
            if (!canAddLight)
                ImGui::EndDisabled();
            if (!canAddLight && GContext->gpuScene)
                ImGui::SetItemTooltip("GPU light buffer capacity reached (%u lights)", lightCapacity);
            ImGui::SameLine();
            bool canDeleteLight = GEditor.selectedLight >= 0 && GEditor.selectedLight < static_cast<int>(lights.size());
            if (!canDeleteLight)
                ImGui::BeginDisabled();
            if (ImGui::Button("Delete Selected Light"))
            {
                lights.erase(lights.begin() + GEditor.selectedLight);
                if (GEditor.selectedLight >= static_cast<int>(lights.size()))
                    GEditor.selectedLight = static_cast<int>(lights.size()) - 1;
                anyChanged = true;
            }
            if (!canDeleteLight)
                ImGui::EndDisabled();
            ImGui::Separator();
            for (int i = 0; i < static_cast<int>(lights.size()); i++)
            {
                auto& light = lights[i];
                ImGui::PushID(i);

                char header[64];
                snprintf(header, sizeof(header), "Light %d (%s)", i, kLightTypeNames[static_cast<int>(light.type)]);
                bool isLightSelected = (GEditor.selectedLight == i);
                ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_DefaultOpen;
                if (isLightSelected)
                    headerFlags |= ImGuiTreeNodeFlags_Selected;
                bool headerOpen = ImGui::CollapsingHeader(header, headerFlags);
                if (ImGui::IsItemClicked())
                {
                    GEditor.selectedLight = i;
                    GEditor.selectedInstance = -1; // deselect instance when selecting light
                    GEditor.selectedMaterial = -1;
                }
                if (headerOpen)
                {
                    bool lightChanged = false;

                    // Type selector
                    int typeInt = static_cast<int>(light.type);
                    if (ImGui::Combo("Type", &typeInt, kLightTypeNames, kLightTypeCount))
                    {
                        light.type = static_cast<FLightType>(typeInt);
                        lightChanged = true;
                    }

                    // Color + Power
                    lightChanged |= ImHDRColorEdit("Color", light.color, light.power);

                    // Direction (Euler angles) for lights with orientation
                    bool hasDirection = (light.type == FLightType::Directional || light.type == FLightType::Spot ||
                                         light.type == FLightType::Disk || light.type == FLightType::Rect);
                    if (hasDirection)
                    {
                        // Decompose quaternion → Euler yaw/pitch (YXZ intrinsic order).
                        // Convention: default forward is (0,0,-1), Y-up.
                        // rotation = rotateY(yaw) * rotateX(pitch)
                        // Extract pitch and yaw directly from the quaternion to avoid
                        // direction-vector round-trip instabilities at the poles.
                        float sinP = 2.0f *
                            (light.transform.rotation.w * light.transform.rotation.x -
                             light.transform.rotation.y * light.transform.rotation.z);
                        sinP = std::clamp(sinP, -1.0f, 1.0f);
                        float pitch = degrees(std::asin(sinP));

                        float sinY = 2.0f *
                            (light.transform.rotation.w * light.transform.rotation.y +
                             light.transform.rotation.x * light.transform.rotation.z);
                        float cosY = 1.0f -
                            2.0f *
                                (light.transform.rotation.x * light.transform.rotation.x +
                                 light.transform.rotation.y * light.transform.rotation.y);
                        float yaw = degrees(std::atan2(sinY, cosY));

                        bool dirChanged = false;
                        dirChanged |= ImGui::SliderFloat("Pitch", &pitch, -90.0f, 90.0f, "%.1f deg");
                        dirChanged |= ImGui::SliderFloat("Yaw", &yaw, -180.0f, 180.0f, "%.1f deg");
                        if (dirChanged)
                        {
                            // Reconstruct quaternion directly: rotateY(yaw) * rotateX(pitch)
                            quat yawQ = angleAxis(radians(yaw), vec3(0, 1, 0));
                            quat pitchQ = angleAxis(radians(pitch), vec3(1, 0, 0));
                            light.transform.rotation = normalize(yawQ * pitchQ);
                            lightChanged = true;
                        }
                    }

                    // Position for positional lights
                    bool hasPosition = (light.type == FLightType::Point || light.type == FLightType::Spot ||
                                        light.type == FLightType::Disk || light.type == FLightType::Rect);
                    if (hasPosition)
                    {
                        lightChanged |= ImGui::DragFloat3("Position", &light.transform.transform.x, 0.1f);
                    }

                    // Range for Point and Spot
                    if (light.type == FLightType::Point || light.type == FLightType::Spot)
                    {
                        lightChanged |= ImGui::DragFloat("Range", &light.range, 0.1f, 0.0f, 1000.0f, "%.2f (0=inf)");
                    }

                    // Spot cone angles
                    if (light.type == FLightType::Spot)
                    {
                        float innerDeg = degrees(light.spotInnerConeAngle);
                        float outerDeg = degrees(light.spotOuterConeAngle);
                        bool coneChanged = false;
                        coneChanged |= ImGui::SliderFloat("Inner Cone", &innerDeg, 0.0f, outerDeg, "%.1f deg");
                        coneChanged |= ImGui::SliderFloat("Outer Cone", &outerDeg, innerDeg, 90.0f, "%.1f deg");
                        if (coneChanged)
                        {
                            light.spotInnerConeAngle = radians(innerDeg);
                            light.spotOuterConeAngle = radians(outerDeg);
                            lightChanged = true;
                        }
                    }

                    // Disk/Rect extents
                    if (light.type == FLightType::Disk || light.type == FLightType::Rect)
                    {
                        lightChanged |= ImGui::DragFloat("Width", &light.width, 0.01f, 0.001f, 100.0f, "%.3f");
                        lightChanged |= ImGui::DragFloat("Height", &light.height, 0.01f, 0.001f, 100.0f, "%.3f");
                    }

                    // Two-sided toggle for area lights
                    if (light.type == FLightType::Disk || light.type == FLightType::Rect)
                    {
                        lightChanged |= ImGui::Checkbox("Two-Sided", &light.twoSided);
                        ImGui::SameLine();
                        lightChanged |= ImGui::Checkbox("Normalize", &light.normalize);
                    }

                    if (lightChanged)
                        anyChanged = true;

                    ImGui::Separator();
                }
                ImGui::PopID();
            }
        }

        // ---- Ambient / Environment ----
        if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
        {
            bool envChanged = ImHDRColorEdit("Ambient", GEditor.shaderGlobals.ambientColor, GEditor.shaderGlobals.ambientPower);

            ImGui::Separator();
            bool hasEnv = GContext->gpuScene && GContext->gpuScene->HasEnvMap();
            ImGui::Text(hasEnv ? "HDRI Loaded" : "No HDRI");
            if (hasEnv)
            {
                DrawTexturePreview("HDRI", GContext->gpuScene->GetEnvMapIndexOrDefault());
                envChanged |= ImGui::SliderFloat("Azimuth Offset", &GEditor.shaderGlobals.envAzimuthOffset, -180.0f, 180.0f,
                                              "%.1f deg");
                bool envEnabled = GEditor.shaderGlobals.useEnvMap != 0u;
                if (ImGui::Checkbox("Enable Env Map", &envEnabled))
                {
                    GEditor.shaderGlobals.useEnvMap = envEnabled ? 1u : 0u;
                    envChanged = true;
                }
            }
            if (envChanged)
            {
                if (GEditor.HasScene())
                {
                    auto& globals = GEditor.Scene().GetSceneGlobals();
                    globals.color = GEditor.shaderGlobals.ambientColor;
                    globals.strength = GEditor.shaderGlobals.ambientPower;
                    globals.azimuthOffset = GEditor.shaderGlobals.envAzimuthOffset;
                }
                anyChanged = true;
            }
            ImGui::TextDisabled("Drag & drop .hdr/.hdri to load");
        }

        if (anyChanged)
        {
            UpdateSceneLights();
            GEditor.shaderGlobals.ptAccumulatedFrames = 0;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // Draw light shape overlays and ImGuizmo manipulator
    DrawLightGizmos();
}

void FRunningImGui()
{
    uint64_t texturePreviewFrame = ++TexturePreviewFrame();
    auto* renderer = GContext->renderer;
    float gpuTimingRes;
    auto timings = renderer->DbgProfilePassTiming(renderer->GetSync(), gpuTimingRes);
    // ImGui
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Camera"))
    {
        ImGui::TextUnformatted(FArcballCamera::kControlsText);
        ImGui::Separator();
        GEditor.cameraUpdated |= ImGui::SliderFloat3("Cam Center", &GEditor.camera.center.x, -50.0f, 50.0f);
        GEditor.cameraUpdated |= ImGui::SliderFloat("Cam Radius", &GEditor.camera.radius, 0.0f, 100.0f);
        GEditor.cameraUpdated |= ImGui::SliderAngle("Cam FOV Y", &GEditor.camera.fovY);
        ImGui::SliderFloat("Exposure (EV)", &GEditor.shaderGlobals.camEV, -16.0f, 16.0f);
        ImGui::Separator();
        ImGui::SliderFloat("WASD Speed", &GEditor.camera.moveSpeed, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        GEditor.cameraUpdated |= ImGui::Checkbox("Enable DOF", &GEditor.aperture.dofEnabled);
        if (GEditor.aperture.dofEnabled)
        {
            GEditor.cameraUpdated |=
                ImGui::SliderFloat("F-Stop", &GEditor.aperture.fStop, 0.1f, 128.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            GEditor.cameraUpdated |= ImGui::SliderFloat("Sensor Height", &GEditor.aperture.sensorHeightMm, 1.0f, 100.0f,
                                                        "%.2f mm", ImGuiSliderFlags_Logarithmic);
            GEditor.cameraUpdated |= ImGui::SliderFloat("Focal Distance", &GEditor.shaderGlobals.focalDistance, 0.1f, 1000.0f, "%.3f",
                                                ImGuiSliderFlags_Logarithmic);
            int apertureBlades = static_cast<int>(GEditor.shaderGlobals.apertureBlades);
            if (ImGui::SliderInt("Blades", &apertureBlades, 0, 16))
            {
                GEditor.shaderGlobals.apertureBlades = static_cast<uint32_t>(apertureBlades);
                GEditor.cameraUpdated = true;
            }
            GEditor.cameraUpdated |= ImGui::SliderAngle("Rotation", &GEditor.shaderGlobals.apertureRotation, -180.0f, 180.0f,
                                                        "%.1f deg");
            GEditor.cameraUpdated |=
                ImGui::SliderFloat("Ratio", &GEditor.shaderGlobals.apertureRatio, 0.01f, 16.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
            float apertureRadiusMm =
                ApertureRadiusFromFStop(GEditor.aperture.fStop, GEditor.aperture.sensorHeightMm, GEditor.camera.fovY);
            ImGui::Text("Aperture Radius: %.3f mm", apertureRadiusMm);
            DrawAperturePreview(GEditor.shaderGlobals.apertureBlades, GEditor.shaderGlobals.apertureRotation,
                                GEditor.shaderGlobals.apertureRatio);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Profiler"))
    {
        if (timings.empty())
        {
            ImGui::Text("No Info");
        }
        else
        {
            if (ImGui::TreeNodeEx("Device", ImGuiTreeNodeFlags_DefaultOpen))
            {
                {
                    size_t used, budget;
                    GContext->device->QueryBudget(used, budget);
                    size_t blockBytes, allocationBytes;
                    GContext->device->QueryAllocationStats(blockBytes, allocationBytes);
                    static String name;
                    if (name.empty())
                        name = GContext->device->QueryDeviceString();
                    ImGui::Text("%s", name.c_str());
                    ImGui::Text("GPU Memory Usage: %.1f MB / %.1f MB", BytesToMiB(used), BytesToMiB(budget));
                    ImGui::Text("GPU Heap: %.1f MB, Blocks: %.1f MB, Slack: %.1f MB",
                                BytesToMiB(allocationBytes), BytesToMiB(blockBytes),
                                BytesToMiB(blockBytes >= allocationBytes ? blockBytes - allocationBytes : 0));
                }
                {
                    Allocator* scratch = GContext->editorFrameScratch ? GContext->editorFrameScratch.get() : GLOBAL_ALLOC;
                    RHIDeviceMemoryStats memoryStats(scratch);
                    GContext->device->QueryMemoryStats(memoryStats);
                    ImGui::Text("VMA Total: %.1f MB alloc, %.1f MB blocks, %u allocations",
                                BytesToMiB(memoryStats.total.allocationBytes),
                                BytesToMiB(memoryStats.total.blockBytes), memoryStats.total.allocationCount);
                    if (ImGui::TreeNodeEx("VMA Heaps", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        DrawRHIDeviceHeapStatsTable(memoryStats);
                        ImGui::TreePop();
                    }
                    if (ImGui::TreeNodeEx("VMA Memory Types", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        DrawRHIDeviceMemoryTypeStatsTable(memoryStats);
                        ImGui::TreePop();
                    }
                }
                {
                    size_t used, budget;
                    GLOBAL_ALLOC->QueryBudget(used, budget);
                    size_t heapUsage = GLOBAL_ALLOC->QueryHeapUsage();
                    ImGui::Text("CPU RSS Memory: %.1f MB", BytesToMiB(used));
                    ImGui::Text("CPU Heap Usage: %.1f MB", BytesToMiB(heapUsage));
                }
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("GPU Scene", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (GContext->gpuScene)
                {
                    Allocator* scratch = GContext->editorFrameScratch ? GContext->editorFrameScratch.get() : GLOBAL_ALLOC;
                    Vector<GPUScene::MemoryStat> stats(scratch);
                    GContext->gpuScene->DbgGetMemoryStatistics(stats);
                    size_t totalBytes = DrawMemoryStatsTable("##GPUSceneMemoryTable", stats);
                    ImGui::Text("Tracked Total: %.1f MB", totalBytes / static_cast<float>(1 << 20u));
                }
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Render Graph", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (GContext->renderer)
                {
                    Allocator* scratch = GContext->editorFrameScratch ? GContext->editorFrameScratch.get() : GLOBAL_ALLOC;
                    Vector<Renderer::MemoryStat> stats(scratch);
                    GContext->renderer->DbgGetMemoryStatistics(stats);
                    size_t totalBytes = DrawMemoryStatsTable("##RenderGraphMemoryTable", stats);
                    ImGui::Text("Renderer-Owned Total: %.1f MB", totalBytes / static_cast<float>(1 << 20u));
                    DrawRenderGraphTexturePreviews(GContext->renderer, scratch);
                }
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx("Frametime", ImGuiTreeNodeFlags_DefaultOpen))
            {
                Allocator* frameScratch = GContext->editorFrameScratch ? GContext->editorFrameScratch.get() : GLOBAL_ALLOC;
                static constexpr size_t kHistogramSamples = 5e3, kFrametimeSamples = 3e2;
                static Vector<ImProfilerSample> samples(GLOBAL_ALLOC);
                static Vector<ImProfilerHistogram> histograms(GLOBAL_ALLOC);
                static ImProfilerHistogram frametime(kFrametimeSamples, GLOBAL_ALLOC);
                static bool pause = false;
                static float presentTimingMS = 0.0f;
                static float gpuTimingMS = 0.0f;
                static int lanes = 0;
                float frametimeAvg = frametime.mean * 1e-6f;
                ImGui::Text("CPU to Present: %.3fms\nP2P: %.3fms (%.1f FPS)\nGPU: %.3fms\n"
                            "CPU/GPU dt: %.3fms",
                            presentTimingMS, frametimeAvg * 1e3f, 1 / frametimeAvg, gpuTimingMS,
                            frametimeAvg * 1e3f - gpuTimingMS);
                auto ClearHistogramData = []
                {
                    for (auto& hist : histograms)
                        hist.clear();
                };
                if (ImModalButton(pause ? "Resume" : "Pause", 0, 2))
                    pause = !pause;
                if (ImModalButton("Flush", 1, 2))
                    ClearHistogramData();
                if (!pause)
                {
                    const size_t passCount = timings.size() / 2;
                    samples.clear();
                    samples.reserve(passCount);
                    histograms.reserve(passCount);
                    for (size_t i = 0; i < passCount; i++)
                    {
                        auto const& pass = renderer->GetTrackedPass(i);
                        ImProfilerSample sample{
                            .id = static_cast<int>(i),
                            .startTick = timings[i * 2],
                            .endTick = timings[i * 2 + 1],
                            .label = pass.name,
                            .color = pass.queue == RHIDeviceQueueType::Graphics ? ImColor(1.0f, 0.5f, 0.0f, 1.0f)
                                                                                : ImColor(0.0f, 0.5f, 0.0f, 1.0f),
                        };
                        samples.emplace_back(std::move(sample));
                        while (histograms.size() <= i)
                            histograms.emplace_back(kHistogramSamples, GLOBAL_ALLOC);
                        histograms[i].push(sample.endTick - sample.startTick);
                    }
                    float presentTimingRes;
                    lanes = ImProfilerAssignLanes(samples, frameScratch);
                    gpuTimingMS = (samples.back().endTick - samples.front().startTick) * 1e-6;
                    gpuTimingMS *= gpuTimingRes;
                    presentTimingMS = renderer->DbgProfilePresentTiming(renderer->GetSync(), presentTimingRes) * 1e-6;
                    presentTimingMS *= presentTimingRes;
                    frametime.push(ImGui::GetIO().DeltaTime * 1e6f);
                }
                int selectedID = -1;
                static int maxLanes = 0;
                ImProfilerDrawTimestampLabel(samples, gpuTimingRes, 8u);
                for (int lane = 0; lane < std::max(maxLanes, lanes); lane++)
                    selectedID = std::max(selectedID, ImProfilerDrawLane(samples, lane));
                maxLanes = std::max(maxLanes, lanes);
                if (ImGui::TreeNode("Tables"))
                {
                    Ranges::sort(samples, [](auto const& a, auto const& b) { return a.id < b.id; });
                    selectedID = std::max(selectedID, ImProfilerDrawTable(samples, gpuTimingRes));
                    ImGui::TreePop();
                }
                if (selectedID >= 0)
                {
                    ImGui::SetNextWindowSize({ImGui::GetWindowSize().x * 0.75f, 0});
                    if (ImGui::BeginTooltip())
                    {
                        if (!pause)
                            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                                               "Profiler still running. Please Pause for accurate Histogram data.");
                        auto it = Ranges::find_if(samples,
                                                  [selectedID](auto const& sample) { return sample.id == selectedID; });
                        ImGui::SeparatorText(it->label.c_str());
                        if (it != samples.end())
                        {
                            auto& hist = histograms[selectedID];
                            Vector<unsigned> bins(frameScratch);
                            hist.bin(bins, 256, false /* log */);
                            float mean = hist.mean * gpuTimingRes * 1e-6f,
                                  median = hist.sorted[hist.sorted.size() / 2] * gpuTimingRes * 1e-6f,
                                  stddev = hist.stddev() * gpuTimingRes * 1e-6f;
                            ImGui::Text("Mean: %.3fms | Median: %.3fms | σ: %.3fms", mean, median, stddev);
                            ImProfilerDrawHistogram(bins, hist, 8, gpuTimingRes, false /* log */);
                        }
                        ImGui::EndTooltip();
                    }
                }
                ImGui::TreePop();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    FLightingPanel();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Rendering"))
    {
        bool changed = false;
        ImGui::SeparatorText("Display");
        bool viewLUTChanged = false;
        auto viewLUTCombo = [](const char* label, int& index, String& externalPath,
                               const ViewLUTEntry* entries, int count)
        {
            const int externalIndex = count;
            if (index < 0 || index > externalIndex || (index == externalIndex && externalPath.empty()))
                index = std::clamp(index, 0, count - 1);

            bool selected = false;
            const char* preview = index == externalIndex ? kExternalViewLUTLabel : entries[index].label;
            bool comboOpen = ImGui::BeginCombo(label, preview);
            if (index == externalIndex && !externalPath.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", externalPath.c_str());
            if (comboOpen)
            {
                for (int i = 0; i < count; ++i)
                {
                    const bool isSelected = i == index;
                    if (ImGui::Selectable(entries[i].label, isSelected))
                    {
                        index = i;
                        selected = true;
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                const bool isExternalSelected = index == externalIndex;
                if (ImGui::Selectable(kExternalViewLUTLabel, isExternalSelected))
                {
                    String selectedPath;
                    if (OpenViewLUTDialog(selectedPath))
                    {
                        externalPath = selectedPath;
                        index = externalIndex;
                        selected = true;
                    }
                }
                if (isExternalSelected)
                    ImGui::SetItemDefaultFocus();
                ImGui::EndCombo();
            }
            return selected;
        };
        viewLUTChanged |= viewLUTCombo("SDR LUT", GEditor.viewLUTSdrIndex, GEditor.viewLUTSdrExternalPath,
                                       kViewLUTsSdr, kViewLUTSdrCount);
        viewLUTChanged |= viewLUTCombo("HDR LUT", GEditor.viewLUTHdrIndex, GEditor.viewLUTHdrExternalPath,
                                       kViewLUTsHdr, kViewLUTHdrCount);
        if (viewLUTChanged)
            ApplyViewLUTSelection();
        if (GContext->windowHDR.propertiesAvailable)
        {
            bool hdrOutput = GContext->enableHDR;
            ImGui::BeginDisabled();
            ImGui::Checkbox("Enable HDR", &hdrOutput);
            ImGui::EndDisabled();
            ImGui::TextDisabled("HDR output follows SDL window HDR state");
        }
        else if (ImGui::Checkbox("Enable HDR", &GContext->enableHDR))
        {
            GEditor.state = FERunningEnter;
            changed = true;
        }
        if (GContext->windowHDR.propertiesAvailable)
        {
            ImGui::Text("SDL HDR: %s", GContext->windowHDR.enabled ? "enabled" : "disabled");
            ImGui::Text("SDR white: %.3f linear (~%.1f nits)", GContext->windowHDR.sdrWhiteLevel,
                        GContext->windowHDR.sdrWhiteNits);
            ImGui::Text("HDR headroom: %.2fx (~%.1f nits peak)", GContext->windowHDR.headroom,
                        GContext->windowHDR.peakNits);
        }
        else
        {
            ImGui::TextDisabled("SDL HDR window properties unavailable");
        }
        if (GEditor.rendererMode == ERendererMode::PathTracer)
        {
            ImGui::SeparatorText("Stats");
            // Throughput: measure delta on the *frame* (dispatch) counter every ~250ms,
            // then derive samples/sec from frames/sec via the spp/tile ratio.
            //   frames  = ptAccumulatedFrames / PTSamplesPerDispatch   (dispatch count)
            //   samples = frames * PTSamplesPerDispatch / PTTileSampleCount
            //           = ptAccumulatedFrames / PTTileSampleCount      (== PTCompletedPixelSamples)
            // i.e. one dispatch advances `spp / tile^2` pixel-samples; spp>1 and tiled
            // dispatch are both handled by this single ratio.
            static double   sLastSampleTime  = ImGui::GetTime();
            static uint32_t sLastFrameCount  = 0u;
            static float    sFramesPerSec    = 0.0f;
            uint32_t samplesPerDispatch = PTSamplesPerDispatch(GEditor.shaderGlobals);
            uint32_t tileSampleCount    = PTTileSampleCount(GEditor.shaderGlobals);
            uint32_t frameCount         = GEditor.shaderGlobals.ptAccumulatedFrames / samplesPerDispatch;
            uint32_t completedSamples   = PTCompletedPixelSamples(GEditor.shaderGlobals);
            double now = ImGui::GetTime();
            double dt  = now - sLastSampleTime;
            if (dt >= 0.25)
            {
                int   deltaFrames = static_cast<int>(frameCount) - static_cast<int>(sLastFrameCount);
                float instFps     = deltaFrames > 0 ? static_cast<float>(deltaFrames) / static_cast<float>(dt) : 0.0f;
                // Light EMA smoothing
                sFramesPerSec   = sFramesPerSec * 0.6f + instFps * 0.4f;
                sLastSampleTime = now;
                sLastFrameCount = frameCount;
            }
            // Reset measurement when accumulation got reset (camera/UI op).
            if (frameCount < sLastFrameCount)
            {
                sLastFrameCount = frameCount;
                sLastSampleTime = now;
                sFramesPerSec   = 0.0f;
            }
            float samplesPerSec = sFramesPerSec * static_cast<float>(samplesPerDispatch) /
                                  static_cast<float>(tileSampleCount);
            ImGui::Text("samples: %u (%.2f sps), frames: %u (%.1f fps)",
                        completedSamples, samplesPerSec, frameCount, sFramesPerSec);
            // Auto-Pause sample limit slider. 0 = disabled.
            int limit = GEditor.renderTask.autoPauseSampleLimit;
            if (ImGui::SliderInt("Auto-Pause Samples", &limit, 0, 65536, limit > 0 ? "%d" : "Off",
                                 ImGuiSliderFlags_Logarithmic))
            {
                GEditor.renderTask.autoPauseSampleLimit = std::max(0, limit);
                // Re-arm auto-pause if user raised the limit above current count
                if (GEditor.renderTask.renderAutoPaused &&
                    GEditor.renderTask.autoPauseSampleLimit > static_cast<int>(completedSamples))
                {
                    GEditor.renderTask.renderAutoPaused = false;
                    GEditor.renderTask.renderPaused = false;
                }
            }
            ImGui::SeparatorText("Path Tracer");
            if (ImModalButton("Fast", 0, 3))
            {
                GEditor.shaderGlobals.ptMaxBouncesDiffuse = 4;
                GEditor.shaderGlobals.ptMaxBouncesSpecular = 4;
                GEditor.shaderGlobals.ptMaxBouncesTransmission = 12;
                GEditor.shaderGlobals.ptFireflyClamp = 1.0f;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            if (ImModalButton("Full", 1, 3))
            {
                GEditor.shaderGlobals.ptMaxBouncesDiffuse = 32;
                GEditor.shaderGlobals.ptMaxBouncesSpecular = 32;
                GEditor.shaderGlobals.ptMaxBouncesTransmission = 32;
                GEditor.shaderGlobals.ptFireflyClamp = 2.0f;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            if (ImModalButton("Über", 2, 3))
            {
                GEditor.shaderGlobals.ptMaxBouncesDiffuse = 100;
                GEditor.shaderGlobals.ptMaxBouncesSpecular = 100;
                GEditor.shaderGlobals.ptMaxBouncesTransmission = 100;
                GEditor.shaderGlobals.ptFireflyClamp = 100.0f;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::SeparatorText("Performance");
            const bool serSupported = GContext->device->GetCapabilities().shaderExecutionReordering;
            bool serEnabled = serSupported && GEditor.rendererConfig.ptShaderExecutionReordering;
            ImGui::BeginDisabled(!serSupported);
            if (ImGui::Checkbox("Shader Execution Reordering", &serEnabled))
            {
                GEditor.rendererConfig.ptShaderExecutionReordering = serEnabled;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            ImGui::EndDisabled();
            if (!serSupported)
                ImGui::TextDisabled("SER is not supported by this device.");
            if (ImGui::Checkbox("Force Texture LOD 0", &GEditor.rendererConfig.forceTextureLOD0))
            {
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            ImGui::SeparatorText("Ray Bounce");
            ImGui::SliderInt("Diffuse", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesDiffuse), 0, 64);
            ImGui::SliderInt("Specular", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesSpecular), 0, 64);
            ImGui::SliderInt("Transmission", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesTransmission), 0, 64);
            ImGui::SeparatorText("Sampling");
            ImGui::SliderFloat("Max Energy", &GEditor.shaderGlobals.ptFireflyClamp, 1.0f, 100.0f, "%.1f");
            static int ptSPPIndex = 3; // 1/25 SPP
            ptSPPIndex = PTSPPOptionIndex(GEditor.shaderGlobals);
            if (ImGui::SliderInt("SPP", &ptSPPIndex, 0, kPTSPPOptionCount - 1,
                                 kPTSPPOptions[ptSPPIndex].label, ImGuiSliderFlags_AlwaysClamp))
                SetPTSPPOption(ptSPPIndex);
            const char* samplerItems[] = {"PCG (Independent)", "Sobol (Quasi-Monte Carlo)"};
            int ptSampler = static_cast<int>(GEditor.rendererConfig.ptSampler);
            if (ImGui::Combo("Sampler", &ptSampler, samplerItems, 2))
            {
                GEditor.rendererConfig.ptSampler = static_cast<uint32_t>(ptSampler);
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            const char* lightSamplerItems[] = { "Uniform", "Power" };
            if (ImGui::Combo("Light Sampler", reinterpret_cast<int*>(&GContext->gpuScene->mLightSamplerType), lightSamplerItems, 2))
            {
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
        }
        if (GEditor.rendererMode == ERendererMode::Raster)
        {
            ImGui::SeparatorText("Rasterizer");
            static float lodLogThreshold = 3;
            ImGui::SliderFloat("LOD ", &lodLogThreshold, 0, 8);
            GEditor.shaderGlobals.lodThreshold = std::pow(10.0f, -lodLogThreshold);
            ImGui::SeparatorText("Performance");
            changed |= ImGui::Checkbox("Force Texture LOD 0", &GEditor.rendererConfig.forceTextureLOD0);
            {
                const char* items[] = {"Overdraw", "Meshlet", "Material ID", "Texture LOD"};
                const unsigned values[] = {kViewOverdraw, kViewMeshlet, kViewMaterialID, kViewTextureLOD};
                ImGui::SeparatorText("Debug View");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */);
            }
            {
                const char* items[] = {"RT Shadows"};
                const unsigned values[] = {kEnableRasterRTShadows};
                ImGui::SeparatorText("Options");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values);
            }
            {
                const char* items[] = {"Frustum", "Occlusion"};
                const unsigned values[] = {kCullFrustum, kCullOcclusion};
                ImGui::SeparatorText("Culling");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.cullFlags, items, values);
            }
        }
        if (GEditor.rendererMode == ERendererMode::PathTracer)
        {
            {
                const char* items[] = {"Diffuse Buffer", "Specular Buffer"};
                const unsigned values[] = {kViewAOVDiffuse, kViewAOVSpecular};
                ImGui::SeparatorText("AOV View");
                if (ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */))
                {
                    GEditor.rendererConfig.viewFlags &= ~kViewTextureLOD;
                    changed = true;
                }
            }
            {
                const char* items[] = {"Texture LOD"};
                const unsigned values[] = {kViewTextureLOD};
                ImGui::SeparatorText("Debug View");
                if (ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */))
                {
                    GEditor.rendererConfig.viewFlags &= ~(kViewAOVDiffuse | kViewAOVSpecular);
                    changed = true;
                }
            }
        }
        if (GEditor.rendererMode == ERendererMode::Raster)
        {
            const char* items[] = {"Position", "BaseColor", "Normal"};
            const unsigned values[] = {kViewPosition, kViewBaseColor, kViewNormal};
            ImGui::SeparatorText("GBuffer View");
            changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */);
        }
        if (changed)
            GEditor.state = FERunningEnter;
    }
    ImGui::End();
    DrawTexturePreviewModal();
    PruneTexturePreviewCache(texturePreviewFrame);
    ImGui::PopStyleColor();
}

void FRendering(RendererHandles const& handles)
{
    auto* renderer = GContext->renderer;
    renderer->BeginExecute();
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();

    bool cancelRendering = false;
    uint32_t targetFrames = static_cast<uint32_t>(GEditor.renderTask.targetSamples);
    // Full-width progress bar at the top
    {
        auto& io = ImGui::GetIO();
        float margin = 16.0f;
        float barH = 28.0f;
        float barW = io.DisplaySize.x - margin * 2.0f;
        ImGui::SetNextWindowPos(ImVec2(margin, margin));
        ImGui::SetNextWindowSize(ImVec2(barW, barH + ImGui::GetStyle().WindowPadding.y * 2.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
        ImGui::Begin("##RenderProgressBar", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize);
        uint32_t completedSamples = GEditor.rendererMode == ERendererMode::PathTracer
            ? PTCompletedPixelSamples(GEditor.shaderGlobals)
            : GEditor.shaderGlobals.ptAccumulatedFrames;
        float fraction = GEditor.renderTask.targetSamples > 0 ? static_cast<float>(completedSamples) /
                static_cast<float>(GEditor.renderTask.targetSamples)
                                                            : 0.0f;
        char overlay[128];
        snprintf(overlay, sizeof(overlay), "%d / %d %s", completedSamples,
                 GEditor.renderTask.targetSamples,
                 GEditor.rendererMode == ERendererMode::PathTracer ? "samples" : "frames");
        ImGui::ProgressBar(fraction, ImVec2(barW, barH), overlay);
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    // Cancel button at the bottom-right corner
    {
        auto& io = ImGui::GetIO();
        const char* cancelLabel = "  Cancel  ";
        ImVec2 textSize = ImGui::CalcTextSize(cancelLabel);
        float btnW = textSize.x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float btnH = textSize.y + ImGui::GetStyle().FramePadding.y * 2.0f;
        float margin = 24.0f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - btnW - margin, io.DisplaySize.y - btnH - margin));
        ImGui::SetNextWindowSize(ImVec2(btnW, btnH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0, 0));
        ImGui::Begin("##RenderCancel", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize);
        cancelRendering = ImGui::Button(cancelLabel, ImVec2(btnW, btnH));
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
    GEditor.shaderGlobals.frameNumber = renderer->GetFrame();
    renderer->ExecuteFrame();
    renderer->EndExecute();
    GEditor.shaderGlobals.ptAccumulatedFrames += PTSamplesPerDispatch(GEditor.shaderGlobals);

    if (cancelRendering || GEditor.shaderGlobals.ptAccumulatedFrames >= targetFrames)
    {
        // Restore spp preview settings
        GEditor.shaderGlobals.ptSamplesPerPixel = GEditor.renderTask.previousSpp;
        GEditor.shaderGlobals.ptDispatchTileSide = GEditor.renderTask.previousSppTile;
        if (!cancelRendering)
            DoRenderReadback(handles);
        GEditor.state = FERunning;
    }
}


// Project a world-space point to screen-space ImVec2
static ImVec2 WorldToScreen(vec3 worldPos, mat4 const& viewProj, ImVec2 displaySize)
{
    vec4 clip = viewProj * vec4(worldPos, 1.0f);
    if (clip.w <= 0.0f)
        return {-1e4f, -1e4f}; // behind camera — off-screen sentinel
    vec3 ndc = vec3(clip) / clip.w;
    return {
        GEditor.viewport.contentMin.x + (ndc.x * 0.5f + 0.5f) * displaySize.x,
        GEditor.viewport.contentMin.y + (-ndc.y * 0.5f + 0.5f) * displaySize.y // flip Y for screen coords
    };
}

// Draw a wireframe circle in world space via ImDrawList
static void DrawWireCircle(ImDrawList* dl, vec3 center, vec3 u, vec3 v, vec2 radius,
                           mat4 const& viewProj, ImVec2 displaySize, ImU32 color, float thickness,
                           int segments = 32)
{
    for (int i = 0; i < segments; i++)
    {
        float a0 = i * 6.2831853f / segments;
        float a1 = (i + 1) * 6.2831853f / segments;
        vec3 p0 = center + u * (cosf(a0) * radius.x) + v * (sinf(a0) * radius.y);
        vec3 p1 = center + u * (cosf(a1) * radius.x) + v * (sinf(a1) * radius.y);
        dl->AddLine(WorldToScreen(p0, viewProj, displaySize),
                    WorldToScreen(p1, viewProj, displaySize), color, thickness);
    }
}

// Draw a line between two world-space points
static void DrawWorldLine(ImDrawList* dl, vec3 a, vec3 b,
                          mat4 const& viewProj, ImVec2 displaySize, ImU32 color, float thickness)
{
    dl->AddLine(WorldToScreen(a, viewProj, displaySize),
                WorldToScreen(b, viewProj, displaySize), color, thickness);
}


static void DrawDirectionalOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));
    float len = 2.0f;

    // Main direction arrow
    DrawWorldLine(dl, pos, pos + dir * len, vp, ds, col, 2.0f);

    // Arrowhead: 3 lines from tip back
    float3 u, v;
    buildOrthonormalBasis(dir, u, v);
    vec3 tip = pos + dir * len;
    for (int i = 0; i < 3; i++)
    {
        float a = i * 2.0943951f; // 120° apart
        vec3 base = tip - dir * 0.3f + (u * cosf(a) + v * sinf(a)) * 0.15f;
        DrawWorldLine(dl, tip, base, vp, ds, col, 2.0f);
    }

    // Small sun-like circle at origin
    DrawWireCircle(dl, pos, u, v, vec2(0.15f), vp, ds, col, 1.5f, 16);
}

static void DrawPointOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    ImVec2 center = WorldToScreen(pos, vp, ds);
    if (center.x < -1e3f) return; // behind camera

    // Compute outer ring radius in pixels.
    // Only the center is projected; radius is derived analytically.
    float outerPx;
    if (light.range > 0.0f)
    {
        float dist = glm::length(pos - GEditor.camera.position);
        float pixelsPerUnit = (ds.y * 0.5f) / (dist * tanf(GEditor.camera.fovY * 0.5f));
        outerPx = std::max(light.range * pixelsPerUnit, 8.0f);
    }
    else
    {
        outerPx = 28.0f; // fixed screen-space size
    }

    // Concentric 2D rings — omnidirectional, no axis bias
    constexpr int kRings = 3;
    for (int i = 0; i < kRings; i++)
    {
        float t = static_cast<float>(i + 1) / kRings;
        float radius = outerPx * t;
        // Fade inner rings slightly
        ImU32 ringCol = (col & 0x00FFFFFF) | (static_cast<ImU32>((col >> 24) * (0.4f + 0.6f * t)) << 24);
        dl->AddCircle(center, radius, ringCol, 32, 1.5f);
    }

    // Small filled dot at center
    dl->AddCircleFilled(center, 3.0f, col, 12);
}

static void DrawSpotOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));
    float coneLen = (light.range > 0.0f) ? light.range : 3.0f;
    float outerR = coneLen * tanf(light.spotOuterConeAngle);

    float3 u, v;
    buildOrthonormalBasis(dir, u, v);
    vec3 tip = pos + dir * coneLen;

    // Base circle at cone end
    DrawWireCircle(dl, tip, u, v, vec2(outerR), vp, ds, col, 1.5f, 24);

    // 4 cone edge lines from apex to base
    for (int i = 0; i < 4; i++)
    {
        float a = i * 1.5707963f; // 90° apart
        vec3 base = tip + (u * cosf(a) + v * sinf(a)) * outerR;
        DrawWorldLine(dl, pos, base, vp, ds, col, 1.5f);
    }

    // Inner cone circle (if different from outer)
    if (light.spotInnerConeAngle > 0.001f)
    {
        float innerR = coneLen * tanf(light.spotInnerConeAngle);
        DrawWireCircle(dl, tip, u, v, vec2(innerR), vp, ds, col & 0x80FFFFFF, 1.0f, 24);
    }
}

static void DrawDiskOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));

    float3 u, v;
    buildOrthonormalBasis(dir, u, v);

    // Disk circle
    DrawWireCircle(dl, pos, u, v, vec2(light.width, light.height), vp, ds, col, 1.5f);

    // Normal arrow
    DrawWorldLine(dl, pos, pos + dir * std::max(light.width, light.height) * 1.5f, vp, ds, col, 2.0f);
}

static void DrawRectOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));

    float3 u, v;
    buildOrthonormalBasis(dir, u, v);

    // Rectangle corners (half-extents)
    vec3 corners[4] = {
        pos + u * light.width + v * light.height,
        pos - u * light.width + v * light.height,
        pos - u * light.width - v * light.height,
        pos + u * light.width - v * light.height,
    };
    for (int i = 0; i < 4; i++)
        DrawWorldLine(dl, corners[i], corners[(i + 1) % 4], vp, ds, col, 1.5f);

    // Normal arrow from center
    DrawWorldLine(dl, pos, pos + dir * 0.5f, vp, ds, col, 2.0f);
}


static void DrawLightGizmos()
{
    if (!GEditor.HasScene())
        return;
    auto lights = GEditor.Scene().GetLights();
    if (lights.empty() || !GEditor.viewport.HasRect())
        return;

    ImVec2 displaySize = GEditor.viewport.Size();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    mat4 viewProj = GEditor.camera.proj * GEditor.camera.view;

    // -- Shape overlays for all lights --
    for (int i = 0; i < static_cast<int>(lights.size()); i++)
    {
        bool selected = (i == GEditor.selectedLight);
        ImU32 color = selected ? IM_COL32(255, 200, 50, 255)   // gold for selected
                               : IM_COL32(255, 255, 100, 100); // dim yellow for others

        auto& light = lights[i];
        switch (light.type)
        {
        case FLightType::Directional: DrawDirectionalOverlay(light, viewProj, drawList, displaySize, color); break;
        case FLightType::Point:       DrawPointOverlay(light, viewProj, drawList, displaySize, color);       break;
        case FLightType::Spot:        DrawSpotOverlay(light, viewProj, drawList, displaySize, color);        break;
        case FLightType::Disk:        DrawDiskOverlay(light, viewProj, drawList, displaySize, color);        break;
        case FLightType::Rect:        DrawRectOverlay(light, viewProj, drawList, displaySize, color);        break;
        }
    }

    // -- ImGuizmo manipulator for the selected light --
    if (GEditor.selectedLight < 0 || GEditor.selectedLight >= static_cast<int>(lights.size()))
        return;

    auto& light = lights[GEditor.selectedLight];
    bool hasPosition = (light.type != FLightType::Directional);

    // Build model matrix from light transform (no scale — lights don't scale)
    mat4 modelMatrix = translate(mat4(1.0f), vec3(light.transform.transform))
                     * mat4_cast(light.transform.rotation);

    ImGuizmo::BeginFrame();
    ImGuizmo::SetDrawlist(drawList);
    ImGuizmo::SetRect(GEditor.viewport.contentMin.x, GEditor.viewport.contentMin.y,
                      displaySize.x, displaySize.y);

    // Directional: rotate only. Others: translate + rotate (never scale).
    ImGuizmo::OPERATION op = static_cast<ImGuizmo::OPERATION>(hasPosition ? GEditor.gizmo.op : ImGuizmo::ROTATE);
    if (op == ImGuizmo::SCALE)
        op = ImGuizmo::TRANSLATE; // lights don't have meaningful uniform scale

    if (ImGuizmo::Manipulate(&GEditor.camera.view[0][0], &GEditor.camera.proj[0][0],
                             op, static_cast<ImGuizmo::MODE>(GEditor.gizmo.mode), &modelMatrix[0][0]))
    {
        float3 newTranslation;
        quat newRotation;
        float3 newScale;
        Math::decompose(modelMatrix, newScale, newRotation, newTranslation);
        light.transform.transform = newTranslation;
        light.transform.rotation = newRotation;
        // Sync to GPU
        UpdateSceneLights();
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
    }
}
