#include <cmath>
#include <cfloat>
#include <algorithm>
#include <nfd.h>
#include <Math/Decompose.hpp>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <Renderer/Postprocess.hpp>
#include <Fonts/PlexSansIcon.h>
#include "EditorState.hpp"
#include <Renderer/Mesh.hpp>

static void DrawLightGizmos();
static void DrawInstanceGizmos();

static float const* GizmoSnapForOperation(ImGuizmo::OPERATION op)
{
    if (!ImGui::GetIO().KeyCtrl)
        return nullptr;

    static float snap[3]{};
    if ((op & ImGuizmo::TRANSLATE) != 0)
    {
        snap[0] = GEditor.gizmo.translateSnap;
        snap[1] = GEditor.gizmo.translateSnap;
        snap[2] = GEditor.gizmo.translateSnap;
    }
    else if ((op & ImGuizmo::ROTATE) != 0)
        snap[0] = GEditor.gizmo.rotateSnap;
    else if ((op & ImGuizmo::SCALE) != 0)
        snap[0] = GEditor.gizmo.scaleSnap;
    else
        return nullptr;
    return snap;
}

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
static constexpr const char* kExternalMatcapLabel = "<external>";

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

static bool OpenMatcapDialog(String& outPath)
{
    nfdu8filteritem_t filters[] = {{"Matcap Image", "png,jpg,jpeg,tga,bmp,dds"}};
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

static int PTSPPOptionIndex(RendererUBO const& ubo)
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

static const char* LightTypeName(FLightType type)
{
    switch (type)
    {
    case FLightType::Directional: return "Directional";
    case FLightType::Point:       return "Point";
    case FLightType::Spot:        return "Spot";
    case FLightType::Disk:        return "Disk";
    case FLightType::Rect:        return "Rect";
    case FLightType::Environment: return "Environment";
    }
    return "Unknown";
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

static void ResetMaterialTexturePickerModal();

void ClearMaterialTexturePreviewCache()
{
    auto& cache = TexturePreviewCache();
    for (auto& entry : cache)
        if (entry.textureID != 0)
            ImGui_ImplFoundation_RemoveImage(entry.textureID);
    cache.clear();
    ResetTexturePreviewModal();
    ResetMaterialTexturePickerModal();
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
        if (ImGui::Button(PSI_REMOVE " Close"))
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
    ImGui::TextDisabled(PSI_ZOOM_IN " Mouse wheel: zoom at cursor, drag: pan");

    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderFloat("Zoom", &state.zoom, 0.05f, 16.0f, "%.2fx");
    ImGui::SameLine();
        if (ImGui::Button(PSI_REFRESH " Reset"))
    {
        state.zoom = 1.0f;
        state.pan = {};
    }
    ImGui::SameLine();
    if (ImGui::Button(PSI_REMOVE " Close"))
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

static bool IsSceneEnvironmentTextureIndex(size_t textureIndex)
{
    if (!GEditor.HasScene())
        return false;
    FLight const* environment = GEditor.Scene().GetEnvironmentLight();
    return environment != nullptr && environment->environmentMap &&
           environment->environmentTexture != kInvalidTexture &&
           textureIndex == environment->environmentTexture;
}

static bool IsPickableSceneTexture2D(FSerializedTexture const& texture)
{
    if (!texture.IsValid())
        return false;
    if (texture.GetDimension() != RHITextureDimension::E2D)
        return false;
    if ((texture.header.caps2 & DDS_CUBEMAP) != 0)
        return false;
    if (texture.GetNumLayers() != 1)
        return false;
    return true;
}

static uint32_t SceneTextureToGpuIndex(uint32_t sceneTextureIndex)
{
    if (sceneTextureIndex == kInvalidTexture || !GContext || !GContext->gpuScene)
        return UINT32_MAX;
    if (sceneTextureIndex >= GEditor.textureIDMap.size())
        return UINT32_MAX;
    TextureHandle const& handle = GEditor.textureIDMap[sceneTextureIndex];
    if (!handle.IsValid() || handle.is3D)
        return UINT32_MAX;
    if (GContext->gpuScene->Query(handle) != GPUScene::Result::Ready)
        return UINT32_MAX;
    return handle.index;
}

struct MaterialTexturePickerModalState
{
    bool pendingOpen{};
    char label[128]{};
    uint32_t* targetSlot{};
    uint32_t pendingSelection{kInvalidTexture};
    ImGui_ImplFoundation_ImageSampler sampler{ImGuiImplFoundationImageSamplerLinear};
};

static constexpr const char* kMaterialTexturePickerModalName = "Select Texture";

static MaterialTexturePickerModalState& MaterialTexturePickerModal()
{
    static MaterialTexturePickerModalState state;
    return state;
}

static void ResetMaterialTexturePickerModal()
{
    auto& state = MaterialTexturePickerModal();
    state.pendingOpen = false;
    state.label[0] = '\0';
    state.targetSlot = nullptr;
    state.pendingSelection = kInvalidTexture;
    state.sampler = ImGuiImplFoundationImageSamplerLinear;
}

static void OpenMaterialTexturePickerModal(const char* label, uint32_t* targetSlot,
                                           ImGui_ImplFoundation_ImageSampler sampler)
{
    auto& state = MaterialTexturePickerModal();
    state.pendingOpen = true;
    std::snprintf(state.label, sizeof(state.label), "%s", label);
    state.targetSlot = targetSlot;
    state.pendingSelection = *targetSlot;
    state.sampler = sampler;
}

static void MaterialTexturePickerTileStyleBegin(bool selected)
{
    if (!selected)
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_NavHighlight));
}

static void MaterialTexturePickerTileStyleEnd(bool selected)
{
    if (!selected)
        return;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

static void MaterialTexturePickerTileSelectionOutline(bool selected)
{
    if (!selected || !ImGui::IsItemVisible())
        return;
    ImVec2 const min = ImGui::GetItemRectMin();
    ImVec2 const max = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_NavHighlight), 0.0f, 0, 2.0f);
}

static void DrawMaterialTexturePickerNoneTile(ImVec2 size, bool selected)
{
    MaterialTexturePickerTileStyleBegin(selected);
    ImGui::InvisibleButton("##NoneTile", size);
    MaterialTexturePickerTileStyleEnd(selected);

    ImVec2 const min = ImGui::GetItemRectMin();
    ImVec2 const max = ImGui::GetItemRectMax();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(min, max, IM_COL32(31, 31, 31, 255));
    char const* label = "(none)";
    ImVec2 const textSize = ImGui::CalcTextSize(label);
    ImVec2 const center{(min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f};
    drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    MaterialTexturePickerTileSelectionOutline(selected);
}

static void DrawMaterialTexturePickerGrid(uint32_t& pendingSelection, ImGui_ImplFoundation_ImageSampler sampler)
{
    Span<FSerializedTexture const> textures = GEditor.Scene().GetTextures();
    float const thumbSize = 64.0f;
    float const thumbPad = 4.0f;
    ImVec2 const thumb{thumbSize, thumbSize};
    ImVec2 const gridAvail = ImGui::GetContentRegionAvail();
    int const columns = std::max(1, static_cast<int>((gridAvail.x + thumbPad) / (thumbSize + thumbPad)));

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));

    int gridIndex = 0;
    auto nextGridColumn = [&]()
    {
        if (gridIndex % columns != 0)
            ImGui::SameLine(0.0f, thumbPad);
        ++gridIndex;
    };

    nextGridColumn();
    ImGui::PushID("None");
    {
        bool const selected = pendingSelection == kInvalidTexture;
        DrawMaterialTexturePickerNoneTile(thumb, selected);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            pendingSelection = kInvalidTexture;
    }
    ImGui::PopID();

    for (size_t textureIndex = 0; textureIndex < textures.size(); ++textureIndex)
    {
        if (IsSceneEnvironmentTextureIndex(textureIndex))
            continue;
        FSerializedTexture const& texture = textures[textureIndex];
        if (!IsPickableSceneTexture2D(texture))
            continue;

        nextGridColumn();
        ImGui::PushID(static_cast<int>(textureIndex));
        bool const selected = pendingSelection == static_cast<uint32_t>(textureIndex);
        MaterialTexturePickerTileStyleBegin(selected);

        uint32_t const gpuIndex = SceneTextureToGpuIndex(static_cast<uint32_t>(textureIndex));
        TexturePreviewImage preview = GetTexturePreviewImage(gpuIndex, sampler);
        if (preview.textureID != 0)
            ImGui::ImageButton("##Thumb", preview.textureID, thumb);
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
            ImGui::Button("...", thumb);
            ImGui::PopStyleColor();
        }

        MaterialTexturePickerTileStyleEnd(selected);
        MaterialTexturePickerTileSelectionOutline(selected);

        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("#%zu  %ux%u", textureIndex, texture.GetWidth(), texture.GetHeight());
            if (preview.textureID == 0)
                ImGui::TextUnformatted("Not resident yet");
            else
                ImGui::TextUnformatted("Double-click to preview");
            ImGui::EndTooltip();
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && preview.textureID != 0)
        {
            char viewerTitle[128];
            std::snprintf(viewerTitle, sizeof(viewerTitle), "%s #%zu", MaterialTexturePickerModal().label, textureIndex);
            OpenTexturePreviewModal(viewerTitle, gpuIndex, sampler, preview);
        }
        else if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
            pendingSelection = static_cast<uint32_t>(textureIndex);

        ImGui::PopID();
    }

    ImGui::PopStyleVar();
}

static bool DrawMaterialTexturePickerModal()
{
    auto& state = MaterialTexturePickerModal();
    if (state.pendingOpen)
    {
        ImGui::OpenPopup(kMaterialTexturePickerModalName);
        state.pendingOpen = false;
    }

    if (!state.targetSlot)
        return false;

    ImGui::SetNextWindowSize(ImVec2(640.0f, 520.0f), ImGuiCond_FirstUseEver);
    bool modalOpen = true;
    if (!ImGui::BeginPopupModal(kMaterialTexturePickerModalName, &modalOpen))
        return false;

    bool committed = false;
    if (!modalOpen)
    {
        ResetMaterialTexturePickerModal();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return false;
    }

    ImGui::TextUnformatted(state.label);
    ImGui::Separator();
    ImGui::BeginChild("MaterialTexturePickerGrid", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.0f), true);
    DrawMaterialTexturePickerGrid(state.pendingSelection, state.sampler);
    ImGui::EndChild();

    if (ImGui::Button(PSI_OK " OK", ImVec2(120.0f, 0.0f)))
    {
        if (*state.targetSlot != state.pendingSelection)
        {
            *state.targetSlot = state.pendingSelection;
            committed = true;
        }
        ResetMaterialTexturePickerModal();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(PSI_REMOVE " Cancel", ImVec2(120.0f, 0.0f)))
    {
        ResetMaterialTexturePickerModal();
        ImGui::CloseCurrentPopup();
    }

    DrawTexturePreviewModal();

    ImGui::EndPopup();
    return committed;
}

static bool IsMaterialTexturePickerOpen()
{
    return MaterialTexturePickerModal().targetSlot != nullptr;
}

static char const* MaterialTexturePropLabel(char* buf, size_t bufSize, char const* name, uint32_t textureIndex)
{
    if (textureIndex != kInvalidTexture)
        std::snprintf(buf, bufSize, "*%s", name);
    else
        std::snprintf(buf, bufSize, "%s", name);
    return buf;
}

static void DrawMaterialTextureSlot(const char* label, uint32_t& sceneTextureIndex,
                                    ImGui_ImplFoundation_ImageSampler sampler = ImGuiImplFoundationImageSamplerLinear)
{
    ImGui::PushID(label);

    ImVec2 const previewSize{48.0f, 48.0f};
    uint32_t const gpuIndex = SceneTextureToGpuIndex(sceneTextureIndex);
    TexturePreviewImage preview = GetTexturePreviewImage(gpuIndex, sampler);
    bool openPicker = false;
    if (preview.textureID != 0)
    {
        if (ImGui::ImageButton("Preview", preview.textureID, previewSize))
            openPicker = true;
    }
    else if (ImGui::Button("None", previewSize))
        openPicker = true;

    if (openPicker)
        OpenMaterialTexturePickerModal(label, &sceneTextureIndex, sampler);

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (sceneTextureIndex == kInvalidTexture)
        ImGui::Text("%s: none", label);
    else if (sceneTextureIndex < GEditor.Scene().GetTextures().size())
    {
        FSerializedTexture const& texture = GEditor.Scene().GetTextures()[sceneTextureIndex];
        ImGui::Text("%s: #%u (%ux%u)", label, sceneTextureIndex, texture.GetWidth(), texture.GetHeight());
    }
    else if (preview.textureID == 0)
        ImGui::Text("%s: #%u (unavailable)", label, sceneTextureIndex);
    else
        ImGui::Text("%s: #%u", label, sceneTextureIndex);

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
    Set<int> visitedViews(scratch);
    renderer->DbgGetTexturePreviews(previews);
    ImGui::SeparatorText(PSI_PICTURE " Textures");
    ImGui::TextDisabled("NOTE: Only resources that can be sampled and interpolated (i.e. non-depth, ID maps) are shown here.");
    if (previews.empty())
    {
        ImGui::TextDisabled("No textures available");
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
        if (visitedViews.contains(item.resourceHandle) || !supported)
            continue;
        visitedViews.insert(item.resourceHandle);

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
        ImGui::DockBuilderSplitNode(dockspaceID, ImGuiDir_Left, 0.25f, &dockLeft, &dockCenter);
        ImGuiID dockRight;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Right, 0.25f, &dockRight, &dockCenter);
            
        ImGuiID dockLeftTop, dockLeftBottom;
        ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Up, 0.25f, &dockLeftTop, &dockLeftBottom);
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
        if (ImGui::BeginMenu(PSI_FOLDER_CLOSE " File"))
        {
            if (ImGui::MenuItem(PSI_FOLDER_OPEN " Scene"))
            {
                nfdu8filteritem_t filters[] = {{"Scene Files", "gltf,glb,fscn"}};
                nfdopendialogu8args_t args = {0};
                args.filterList = filters;
                args.filterCount = 1;
                nfdu8char_t* outPath = nullptr;
                if (NFD_OpenDialogU8_With(&outPath, &args) == NFD_OKAY)
                {
                    RequestLoadScene(outPath);
                    NFD_FreePathU8(outPath);
                }
            }
            if (ImGui::MenuItem(PSI_SUN " HDRI"))
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
            ImGui::EndMenu();
        }

        if (GContext->gpuScene && GContext->gpuScene->GetInstanceCount() != 0)
        {
            if (ImGui::BeginMenu(PSI_CAMERA " Render"))
            {
                if (ImGui::MenuItem(PSI_PICTURE " HDR Image"))
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
                if (ImGui::MenuItem(PSI_PICTURE " SDR Image", nullptr, false, !GContext->enableHDR))
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
                ImGui::EndMenu();
            }
        }

        static bool openHelpPopup = false;
        if (ImGui::BeginMenu(PSI_QUESTION_SIGN " Help"))
        {
            if (ImGui::MenuItem(PSI_INFO_SIGN " Keyboard Shortcuts..."))
                openHelpPopup = true;
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
            if (ImGui::Button(PSI_PLAY " Start Render"))
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
            if (ImGui::Button(PSI_REMOVE " Cancel"))
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (openHelpPopup)
        {
            ImGui::OpenPopup("Keyboard Shortcuts");
            openHelpPopup = false;
        }
        if (ImGui::BeginPopupModal("Keyboard Shortcuts", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted(PSI_CAMERA " Camera");
            ImGui::BulletText("Mouse Left drag: Orbit");
            ImGui::BulletText("Mouse Right drag: Pan");
            ImGui::BulletText("Mouse Wheel: Zoom");
            ImGui::BulletText("W / A / S / D: Fly");
            ImGui::BulletText("Shift: Move faster");
            ImGui::BulletText("Space: Reset orbit center");
            ImGui::Separator();
            ImGui::TextUnformatted(PSI_MOVE " Viewport Gizmo");
            ImGui::BulletText("G: Translate");
            ImGui::BulletText("R: Rotate");
            ImGui::BulletText("Q: Scale");
            ImGui::BulletText("Ctrl (held): Snap while transforming");
            ImGui::Separator();
            ImGui::TextUnformatted(PSI_COG " Interface");
            ImGui::BulletText("Tab: Toggle editor panels");
            if (ImGui::Button(PSI_REMOVE " Close"))
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
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
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
            if (ImGui::Button(" RASTER "))
            {
                if (GEditor.rendererMode != ERendererMode::Raster)
                {
                    GEditor.rendererMode = ERendererMode::Raster;
                    GEditor.state = FERunningEnter;
                }
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
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
            ImGui::TextDisabled("Material %d", materialIndex);
            if (IsSelectedInstanceValid())
                ImGui::SameLine(), ImGui::TextDisabled("| From Instance %d", GEditor.selectedInstance);
            ImGui::Separator();

            bool changed = false;
            if (ImGui::BeginTabBar("MaterialTabs"))
            {
                if (ImGui::BeginTabItem(PSI_ADJUST " Properties"))
                {
                    ImGui::TextDisabled("NOTE: Properties with an asterisk (*) are texture-mapped.");
                    ImGui::Separator();

                    const char* shaderBlockLabels[] = {"Principled", "Hair"};
                    int shaderBlock = static_cast<int>(material.shaderBlockID);
                    if (ImGui::Combo("Shader Block", &shaderBlock, shaderBlockLabels, IM_ARRAYSIZE(shaderBlockLabels)))
                    {
                        material.shaderBlockID = static_cast<FMaterialShaderBlock>(shaderBlock);
                        changed = true;
                    }

                    ImGui::SeparatorText("Principled");
                    char propLabel[64];
                    changed |= ImGui::ColorEdit4(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Base Color", material.baseColorTexture),
                                                 &material.baseColorFactor.x);
                    changed |= ImHDRColorEdit(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Emissive", material.emissiveTexture),
                                              reinterpret_cast<float3&>(material.emissiveFactor), material.emissiveFactor.w /* otherwise unused */);
                    changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Metallic", material.metallicRoughnessTexture),
                                                  &material.metallicFactor, 0.0f, 1.0f, "%.3f");
                    changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Roughness", material.metallicRoughnessTexture),
                                                  &material.roughnessFactor, 0.0f, 1.0f, "%.3f");
                    changed |= ImGui::DragFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Normal Scale", material.normalTexture),
                                                &material.normalScale, 0.01f, -8.0f, 8.0f, "%.3f");
                    changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Transmission", material.transmissionTexture),
                                                  &material.transmissionFactor, 0.0f, 1.0f, "%.3f");
                    changed |= ImGui::SliderFloat("IOR", &material.ior, 1.0f, 3.0f, "%.3f");
                    changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Specular", material.specularTexture),
                                                  &material.specularFactor, 0.0f, 1.0f, "%.3f");
                    changed |= ImGui::DragFloat3(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Specular Color", material.specularColorTexture),
                                                 &material.specularColorFactor.x, 0.01f, 0.0f, FLT_MAX, "%.3f");
                    changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Anisotropy Strength", material.anisotropyTexture),
                                                  &material.anisotropyStrength, 0.0f, 1.0f, "%.3f");
                    changed |= ImGui::DragFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Anisotropy Rotation", material.anisotropyTexture),
                                                &material.anisotropyRotation, 0.01f, -FLT_MAX, FLT_MAX, "%.3f rad");

                    ImGui::SeparatorText("Sheen");
                    float sheenWeight = std::clamp(std::max({material.sheenColorFactor.x, material.sheenColorFactor.y, material.sheenColorFactor.z}), 0.0f, 1.0f);
                    float3 sheenTint = sheenWeight > 1e-6f ? material.sheenColorFactor / sheenWeight : float3{1.0f, 1.0f, 1.0f};
                    bool sheenChanged = false;
                    sheenChanged |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Sheen Weight", material.sheenColorTexture),
                                                       &sheenWeight, 0.0f, 1.0f, "%.3f");
                    sheenChanged |= ImGui::ColorEdit3(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Sheen Tint", material.sheenColorTexture),
                                                      &sheenTint.x);
                    if (sheenChanged)
                    {
                        material.sheenColorFactor = std::clamp(sheenWeight, 0.0f, 1.0f) * Math::clamp(sheenTint, float3{0.0f, 0.0f, 0.0f}, float3{1.0f, 1.0f, 1.0f});
                        changed = true;
                    }
                    changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Sheen Roughness", material.sheenRoughnessTexture),
                                                  &material.sheenRoughnessFactor, 0.0f, 1.0f, "%.3f");

                    ImGui::SeparatorText("Clearcoat");
                    changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Clearcoat Weight", material.clearcoatTexture),
                                                  &material.clearcoatFactor, 0.0f, 1.0f, "%.3f");
                    changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Clearcoat Roughness", material.clearcoatRoughnessTexture),
                                                  &material.clearcoatRoughnessFactor, 0.0f, 1.0f, "%.3f");

                    ImGui::SeparatorText("Hair");
                    changed |= ImGui::SliderFloat("Beta M", &material.hairBetaM, 0.0f, 1.0f, "%.3f");
                    changed |= ImGui::SliderFloat("Beta N", &material.hairBetaN, 0.0f, 1.0f, "%.3f");
                    changed |= ImGui::DragFloat("Alpha", &material.hairAlpha, 0.1f, -20.0f, 20.0f, "%.2f deg");

                    ImGui::SeparatorText("Subsurface");
                    changed |= ImGui::SliderFloat("Weight", &material.subsurfaceFactor, 0.0f, 1.0f, "%.3f");
                    changed |= ImGui::ColorEdit3("Color", &material.subsurfaceColor.x);
                    changed |= ImGui::DragFloat3("Radius", &material.subsurfaceRadius.x, 0.001f, 0.0f, FLT_MAX, "%.4f");
                    changed |= ImGui::SliderFloat("Scale", &material.subsurfaceScale, 0.0f, 1.0f, "%.4f");

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(PSI_PICTURE " Textures"))
                {
                    DrawMaterialTextureSlot("Base Color", material.baseColorTexture);
                    DrawMaterialTextureSlot("Emissive", material.emissiveTexture);
                    DrawMaterialTextureSlot("Metallic/Roughness", material.metallicRoughnessTexture);
                    DrawMaterialTextureSlot("Normal", material.normalTexture, ImGuiImplFoundationImageSamplerNearest);
                    DrawMaterialTextureSlot("Transmission", material.transmissionTexture);
                    DrawMaterialTextureSlot("Specular", material.specularTexture);
                    DrawMaterialTextureSlot("Specular Color", material.specularColorTexture);
                    DrawMaterialTextureSlot("Anisotropy", material.anisotropyTexture);
                    DrawMaterialTextureSlot("Sheen Color", material.sheenColorTexture);
                    DrawMaterialTextureSlot("Sheen Roughness", material.sheenRoughnessTexture);
                    DrawMaterialTextureSlot("Clearcoat", material.clearcoatTexture);
                    DrawMaterialTextureSlot("Clearcoat Roughness", material.clearcoatRoughnessTexture);

                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

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
            if (ImGui::RadioButton(PSI_MOVE " Translate (G)", GEditor.gizmo.op == ImGuizmo::TRANSLATE))
                GEditor.gizmo.op = ImGuizmo::TRANSLATE;
            ImGui::SameLine();
            if (ImGui::RadioButton(PSI_REFRESH " Rotate (R)", GEditor.gizmo.op == ImGuizmo::ROTATE))
                GEditor.gizmo.op = ImGuizmo::ROTATE;
            ImGui::SameLine();
            if (ImGui::RadioButton(PSI_RESIZE_FULL " Scale (Q)", GEditor.gizmo.op == ImGuizmo::SCALE))
                GEditor.gizmo.op = ImGuizmo::SCALE;
            if (GEditor.gizmo.op != ImGuizmo::SCALE)
            {
                if (ImGui::RadioButton(PSI_MAP_MARKER " Local", GEditor.gizmo.mode == ImGuizmo::LOCAL))
                    GEditor.gizmo.mode = ImGuizmo::LOCAL;
                ImGui::SameLine();
                if (ImGui::RadioButton(PSI_GLOBE " World", GEditor.gizmo.mode == ImGuizmo::WORLD))
                    GEditor.gizmo.mode = ImGuizmo::WORLD;
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
            if (ImGui::Button(PSI_TRASH " Delete Instance"))
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
    if (GEditor.scrollSelectedLightToTop && GEditor.selectedLight >= 0)
        ImGui::SetWindowFocus("Lighting");

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
        if (ImGui::CollapsingHeader(PSI_SUN " Scene Lights", ImGuiTreeNodeFlags_DefaultOpen))
        {
            GEditor.Scene().EnsureEnvironmentLight();
            auto& lights = GEditor.Scene().mTables.lights;
            uint32_t lightCapacity = GContext->gpuScene ? GContext->gpuScene->GetLightCapacity() : 0u;
            bool canAddLight = GContext->gpuScene && lights.size() < lightCapacity;
            uint32_t gpuLightCount = GContext->gpuScene ? GContext->gpuScene->GetLightCount() : 0u;
            float sceneLightImportanceSum = 0.0f;
            for (uint32_t lightIndex = 0; lightIndex < gpuLightCount; ++lightIndex)
                sceneLightImportanceSum += std::max(0.0f, GContext->gpuScene->GetLight(lightIndex).importance);
            if (!canAddLight)
                ImGui::BeginDisabled();
            if (ImModalButton(canAddLight ? PSI_PLUS_SIGN " Add Light" : "(Lights Full)"))
            {
                FLight light{};
                light.type = FLightType::Point;
                light.transform.transform = GEditor.camera.position;
                light.transform.rotation = GEditor.camera.rot;
                light.color = float3{1.0f, 0.92f, 0.78f};
                light.power = 10.0f;
                light.range = 10.0f;
                size_t const insertIndex = !lights.empty() && lights.front().type == FLightType::Environment ? 1u : 0u;
                lights.insert(lights.begin() + static_cast<std::ptrdiff_t>(insertIndex), light);
                GEditor.selectedLight = static_cast<int>(insertIndex);
                GEditor.selectedInstance = -1;
                GEditor.selectedMaterial = -1;
                GEditor.scrollSelectedLightToTop = true;
                GEditor.selectedLightHighlightStart = static_cast<float>(ImGui::GetTime());
                anyChanged = true;
            }
            if (!canAddLight)
                ImGui::EndDisabled();
            ImGui::Separator();
            static constexpr float kLightHighlightDuration = 1.2f;

            for (int i = 0; i < static_cast<int>(lights.size()); i++)
            {
                auto& light = lights[i];
                bool const isEnvironment = light.type == FLightType::Environment;
                ImGui::PushID(i);

                char header[64];
                snprintf(header, sizeof(header), PSI_BOLT " Light %d (%s)", i, LightTypeName(light.type));
                bool isLightSelected = (GEditor.selectedLight == i);
                ImGuiTreeNodeFlags headerFlags = ImGuiTreeNodeFlags_DefaultOpen;
                if (isLightSelected)
                    headerFlags |= ImGuiTreeNodeFlags_Selected;

                bool pushedHeaderTextColor = false;
                if (isLightSelected && GEditor.selectedLightHighlightStart >= 0.0f)
                {
                    float const elapsed = static_cast<float>(ImGui::GetTime()) - GEditor.selectedLightHighlightStart;
                    float const t = std::clamp(elapsed / kLightHighlightDuration, 0.0f, 1.0f);
                    float const boost = ImEaseInOutCubic(1.0f - t);
                    if (boost > 0.0f)
                    {
                        ImVec4 const base = ImGui::GetStyleColorVec4(ImGuiCol_Header);
                        ImVec4 const bright{1.0f, 1.0f, 1.0f, base.w};
                        ImGui::PushStyleColor(ImGuiCol_Header,
                                              ImVec4(base.x + (bright.x - base.x) * boost,
                                                     base.y + (bright.y - base.y) * boost,
                                                     base.z + (bright.z - base.z) * boost, base.w));
                        pushedHeaderTextColor = true;
                    }
                }

                bool headerOpen = ImGui::CollapsingHeader(header, headerFlags);
                if (pushedHeaderTextColor)
                    ImGui::PopStyleColor();

                if (isLightSelected && GEditor.scrollSelectedLightToTop)
                {
                    ImGui::SetScrollHereY(0.0f);
                    GEditor.scrollSelectedLightToTop = false;
                }
                if (ImGui::IsItemClicked())
                {
                    GEditor.selectedLight = i;
                    GEditor.selectedInstance = -1; // deselect instance when selecting light
                    GEditor.selectedMaterial = -1;
                    GEditor.scrollSelectedLightToTop = true;
                    GEditor.selectedLightHighlightStart = static_cast<float>(ImGui::GetTime());
                }
                if (headerOpen)
                {
                    bool lightChanged = false;

                    // Type selector
                    if (isEnvironment)
                    {
                        ImGui::TextDisabled("Environment light is not removable.");
                    }
                    else
                    {
                        int typeInt = static_cast<int>(light.type);
                        if (ImGui::Combo("Type", &typeInt, kLightTypeNames, kLightTypeCount))
                        {
                            light.type = static_cast<FLightType>(typeInt);
                            lightChanged = true;
                        }
                    }

                    // Color + Power
                    lightChanged |= ImHDRColorEdit(isEnvironment ? "Ambient" : "Color", light.color, light.power);
                    if (GContext->gpuScene && i < static_cast<int>(gpuLightCount))
                    {
                        float importance = GContext->gpuScene->GetLight(static_cast<uint32_t>(i)).importance;
                        float probability = sceneLightImportanceSum > 0.0f ? importance / sceneLightImportanceSum : 0.0f;
                        ImGui::BeginDisabled();
                        ImGui::InputFloat("Importance", &importance, 0.0f, 0.0f, "%.6g");
                        ImGui::InputFloat("Prob", &probability, 0.0f, 0.0f, "%.6g");
                        ImGui::EndDisabled();
                    }

                    if (isEnvironment)
                    {
                        ImGui::Separator();
                        bool hasEnv = GContext->gpuScene && GContext->gpuScene->HasEnvMap();
                        ImGui::Text(hasEnv ? PSI_OK " HDRI Loaded" : PSI_WARNING_SIGN " No HDRI");
                        if (hasEnv)
                        {
                            DrawTexturePreview("HDRI", GContext->gpuScene->GetEnvMapIndexOrDefault());
                            lightChanged |= ImGui::SliderFloat("Azimuth Offset", &light.environmentAzimuthOffset,
                                                              -180.0f, 180.0f, "%.1f deg");
                            bool envEnabled = light.environmentMap;
                            if (ImGui::Checkbox("Enable Env Map", &envEnabled))
                            {
                                light.environmentMap = envEnabled;
                                lightChanged = true;
                            }
                        }
                        ImGui::TextDisabled(PSI_UPLOAD " Drag & drop .hdr/.hdri to load");
                    }

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

                    // Position (gizmo anchor; direction is unchanged for directionals)
                    bool hasPosition = (light.type == FLightType::Directional || light.type == FLightType::Point ||
                                        light.type == FLightType::Spot || light.type == FLightType::Disk ||
                                        light.type == FLightType::Rect);
                    if (hasPosition)
                    {
                        lightChanged |= ImGui::DragFloat3("Position", &light.transform.transform.x, 0.1f);
                    }

                    if (light.type == FLightType::Directional)
                    {
                        float angularDiameter = degrees(light.angularDiameter);
                        ImGui::Separator();
                        lightChanged |=
                            ImGui::SliderFloat("Angular Diameter", &angularDiameter, 0.0f, 180.0f, "%.2f deg");
                        if (lightChanged)
                            light.angularDiameter = radians(angularDiameter);
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

                    if (isEnvironment)
                        ImGui::BeginDisabled();
                    bool removeLight = ImGui::Button(PSI_TRASH " Remove Light");
                    if (isEnvironment)
                    {
                        ImGui::EndDisabled();
                        ImGui::SetItemTooltip("Environment light is required");
                    }
                    if (removeLight)
                    {
                        lights.erase(lights.begin() + i);
                        if (GEditor.selectedLight == i)
                            GEditor.selectedLight = -1;
                        else if (GEditor.selectedLight > i)
                            --GEditor.selectedLight;
                        anyChanged = true;
                        ImGui::PopID();
                        break;
                    }

                    ImGui::Separator();
                }
                ImGui::PopID();
            }
        }

        if (anyChanged)
        {
            UpdateSceneLights();
            GEditor.shaderGlobals.ptAccumulatedFrames = 0;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // ImGuizmo manipulator for the selected light (3D wireframes are drawn in EditorGizmos pass)
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
        const char* projectionItems[] = {"Perspective", "Panoramic (Equirectangular)"};
        int cameraProjection = static_cast<int>(GEditor.shaderGlobals.cameraProjection);
        if (ImGui::Combo("Projection", &cameraProjection, projectionItems, IM_ARRAYSIZE(projectionItems)))
        {
            GEditor.shaderGlobals.cameraProjection = static_cast<uint32_t>(cameraProjection);
            GEditor.cameraUpdated = true;
        }
        bool perspectiveCamera = GEditor.shaderGlobals.cameraProjection == kCameraProjectionPerspective;
        if (perspectiveCamera)
            GEditor.cameraUpdated |= ImGui::SliderAngle("Cam FOV Y", &GEditor.camera.fovY);
        else
            ImGui::TextDisabled("Renders a 360x180 equirectangular view.");
        ImGui::SliderFloat("Exposure (EV)", &GEditor.shaderGlobals.camEV, -16.0f, 16.0f);
        ImGui::Separator();
        ImGui::SliderFloat("WASD Speed", &GEditor.camera.moveSpeed, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        if (perspectiveCamera)
        {
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
            if (ImGui::TreeNodeEx(PSI_SIGNAL " Device", ImGuiTreeNodeFlags_DefaultOpen))
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
            if (ImGui::TreeNodeEx(PSI_SITEMAP " GPU Scene", ImGuiTreeNodeFlags_DefaultOpen))
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
            if (ImGui::TreeNodeEx(PSI_PICTURE " Render Graph", ImGuiTreeNodeFlags_DefaultOpen))
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
            if (ImGui::TreeNodeEx(PSI_TIME " Frametime", ImGuiTreeNodeFlags_DefaultOpen))
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
                if (ImModalButton(pause ? PSI_PLAY " Resume" : PSI_PAUSE " Pause", 0, 2))
                    pause = !pause;
                if (ImModalButton(PSI_REFRESH " Flush", 1, 2))
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
    FAnimationPanel();
    DrawInstanceGizmos();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Rendering"))
    {
        bool changed = false;
        ImGui::SeparatorText(PSI_EYE_OPEN " Display");
        bool viewLUTChanged = false;
        auto viewLUTCombo = [](const char* label, int& index, String& externalPath,
                               Postprocess::ViewLUTDomain domain)
        {
            Span<Postprocess::ViewLUTEntry const> entries = Postprocess::EnumerateViewLUTEntries(domain);
            int const count = static_cast<int>(entries.size());
            const int externalIndex = Postprocess::GetExternalViewLUTIndex(domain);
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
                                       Postprocess::ViewLUTDomain::SDR);
        viewLUTChanged |= viewLUTCombo("HDR LUT", GEditor.viewLUTHdrIndex, GEditor.viewLUTHdrExternalPath,
                                       Postprocess::ViewLUTDomain::HDR);
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
        ImGui::SeparatorText(PSI_PICTURE " Texture Sampling");
        if (ImGui::Checkbox("Anisotropic Filtering", &GEditor.rendererConfig.textureAnisoEnable))
        {
            changed = true;
            if (GEditor.rendererMode == ERendererMode::PathTracer)
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
        }
        {
            float anisoLevel = GEditor.rendererConfig.textureAnisoLevel;
            ImGui::BeginDisabled(!GEditor.rendererConfig.textureAnisoEnable);
            if (ImGui::SliderFloat("Anisotropy Level", &anisoLevel, 1.0f, 16.0f, "%.0f"))
            {
                GEditor.rendererConfig.textureAnisoLevel = anisoLevel;
                changed = true;
                if (GEditor.rendererMode == ERendererMode::PathTracer)
                    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::EndDisabled();
        }
        {
            const char* filterItems[] = {"Bilinear", "Trilinear"};
            int filterMode = GEditor.rendererConfig.textureTrilinear ? 1 : 0;
            if (ImGui::Combo("Texture Filter", &filterMode, filterItems, 2))
            {
                GEditor.rendererConfig.textureTrilinear = filterMode == 1;
                changed = true;
                if (GEditor.rendererMode == ERendererMode::PathTracer)
                    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
        }
        if (GEditor.rendererMode == ERendererMode::PathTracer)
        {
            ImGui::SeparatorText(PSI_SIGNAL " Stats");
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
            ImGui::SeparatorText(PSI_BOLT " Path Tracer");
            if (ImModalButton(PSI_BOLT " Fast", 0, 3))
            {
                GEditor.shaderGlobals.ptMaxBouncesDiffuse = 4;
                GEditor.shaderGlobals.ptMaxBouncesSpecular = 4;
                GEditor.shaderGlobals.ptMaxBouncesTransmission = 12;
                GEditor.shaderGlobals.ptFireflyClamp = GContext->rendererSettings.energyClampOverride; // Default 1.0
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            if (ImModalButton(PSI_FIRE " Full", 1, 3))
            {
                GEditor.shaderGlobals.ptMaxBouncesDiffuse = 32;
                GEditor.shaderGlobals.ptMaxBouncesSpecular = 32;
                GEditor.shaderGlobals.ptMaxBouncesTransmission = 32;
                GEditor.shaderGlobals.ptFireflyClamp = 2.0f;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            if (ImModalButton(PSI_BEAKER " Über", 2, 3))
            {
                GEditor.shaderGlobals.ptMaxBouncesDiffuse = 100;
                GEditor.shaderGlobals.ptMaxBouncesSpecular = 100;
                GEditor.shaderGlobals.ptMaxBouncesTransmission = 100;
                GEditor.shaderGlobals.ptFireflyClamp = 100.0f;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::SeparatorText(PSI_DASHBOARD " Performance");
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
            if (ImGui::Checkbox("Energy Compensation", &GEditor.rendererConfig.energyCompensation))
            {
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::SeparatorText(PSI_RANDOM " Ray Bounce");
            ImGui::SliderInt("Diffuse", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesDiffuse), 0, 64);
            ImGui::SliderInt("Specular", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesSpecular), 0, 64);
            ImGui::SliderInt("Transmission", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBouncesTransmission), 0, 64);
            ImGui::SeparatorText(PSI_RANDOM " Sampling");
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
            const char* lightSamplerItems[] = { "Uniform", "Importance" };
            if (ImGui::Combo("Light Sampler", reinterpret_cast<int*>(&GContext->gpuScene->mLightSamplerType), lightSamplerItems, 2))
            {
                CommitSceneToGPU(true); // Light table needs to be rebuilt
            }
        }
        if (GEditor.rendererMode == ERendererMode::Raster)
        {
            ImGui::SeparatorText(PSI_EYE_OPEN " Rasterizer");
            static float lodLogThreshold = 3;
            ImGui::SliderFloat("LOD ", &lodLogThreshold, 0, 8);
            GEditor.shaderGlobals.lodThreshold = std::pow(10.0f, -lodLogThreshold);
            ImGui::SeparatorText(PSI_DASHBOARD " Performance");
            changed |= ImGui::Checkbox("Force Texture LOD 0", &GEditor.rendererConfig.forceTextureLOD0);
            {
                const char* items[] = {"Overdraw", "Meshlet", "Material ID", "Matcap"};
                const unsigned values[] = {kViewOverdraw, kViewMeshlet, kViewMaterialID, kViewMatcap};
                ImGui::SeparatorText(PSI_EYE_OPEN " Raster Debug View");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */);
            }
            {
                const char* items[] = {"RT Shadows"};
                const unsigned values[] = {kEnableRasterRTShadows};
                ImGui::SeparatorText(PSI_COG " Options");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values);
            }
            if ((GEditor.rendererConfig.viewFlags & kViewMatcap) != 0u)
            {
                ImGui::SeparatorText(PSI_TINT " Matcap");
                if (GEditor.matcapIndex < 0 || GEditor.matcapIndex > kMatcapCount ||
                    (GEditor.matcapIndex == kMatcapCount && GEditor.matcapExternalPath.empty()))
                {
                    GEditor.matcapIndex = std::clamp(GEditor.matcapIndex, 0, kMatcapCount - 1);
                }

                bool matcapChanged = false;
                const char* preview =
                    GEditor.matcapIndex == kMatcapCount
                                          ? kExternalMatcapLabel
                                          : kMatcaps[GEditor.matcapIndex].label;
                if (ImGui::BeginCombo("Matcap Preset", preview))
                {
                    for (int i = 0; i < kMatcapCount; ++i)
                    {
                        bool const selected = i == GEditor.matcapIndex;
                        if (ImGui::Selectable(kMatcaps[i].label, selected))
                        {
                            GEditor.matcapIndex = i;
                            matcapChanged = true;
                        }
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    bool const selectedExternal = GEditor.matcapIndex == kMatcapCount;
                    if (ImGui::Selectable(kExternalMatcapLabel, selectedExternal))
                    {
                        String selectedPath;
                        if (OpenMatcapDialog(selectedPath))
                        {
                            GEditor.matcapExternalPath = selectedPath;
                            GEditor.matcapIndex = kMatcapCount;
                            matcapChanged = true;
                        }
                    }
                    if (selectedExternal)
                        ImGui::SetItemDefaultFocus();
                    ImGui::EndCombo();
                }
                if (GEditor.matcapIndex == kMatcapCount && !GEditor.matcapExternalPath.empty() && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", GEditor.matcapExternalPath.c_str());
                if (matcapChanged)
                    ApplyMatcapSelection();
                DrawTexturePreview("Matcap", GEditor.shaderGlobals.matcapTextureIndex);
            }
            {
                const char* items[] = {"Frustum", "Occlusion"};
                const unsigned values[] = {kCullFrustum, kCullOcclusion};
                ImGui::SeparatorText(PSI_FILTER " Culling");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.cullFlags, items, values);
            }
        }
        if (GEditor.rendererMode == ERendererMode::PathTracer)
        {
            {
                const char* items[] = {"Diffuse Buffer", "Specular Buffer"};
                const unsigned values[] = {kViewAOVDiffuse, kViewAOVSpecular};
                ImGui::SeparatorText(PSI_EYE_OPEN " AOV View");
                if (ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */))
                {
                    GEditor.rendererConfig.viewFlags &= ~kViewTextureLOD;
                    changed = true;
                }
            }
        }
        {
            const char* items[] = {"BaseColor", "Normal", "Position", "Texture LOD"};
            const unsigned values[] = {kViewBaseColor, kViewNormal, kViewPosition, kViewTextureLOD};
            ImGui::SeparatorText(PSI_BUG " Debug View");
            changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */);
        }
        {
            const char* items[] = {"White Base Color"};
            const unsigned values[] = {kMaterialDbgWhiteBaseColor};
            ImGui::SeparatorText(PSI_ADJUST " Material Debug");
            changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.materialFlags, items, values, true /* solo */);
        }
        if (changed)
            GEditor.state = FERunningEnter;
    }
    ImGui::End();
    if (DrawMaterialTexturePickerModal())
        CommitSceneToGPU(true);
    if (!IsMaterialTexturePickerOpen())
        DrawTexturePreviewModal();
    PruneTexturePreviewCache(texturePreviewFrame);
    ImGui::PopStyleColor();
}

void FRendering(RendererOutputs const& outputs)
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
        const char* cancelLabel = PSI_REMOVE " Cancel";
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
    GEditor.postprocessGlobals.camEV = GEditor.shaderGlobals.camEV;
    GEditor.postprocessGlobals.dbgShowOutline = GEditor.showImGui ? 1u : 0u;
    GEditor.postprocessGlobals.ptAccumulatedFrames = GEditor.shaderGlobals.ptAccumulatedFrames;
    GEditor.postprocessGlobals.ptDispatchTileSide = GEditor.shaderGlobals.ptDispatchTileSide;
    GEditor.postprocessGlobals.fbWidth = GEditor.shaderGlobals.fbWidth;
    GEditor.postprocessGlobals.fbHeight = GEditor.shaderGlobals.fbHeight;
    GEditor.postprocessGlobals.viewLutIndex =
        Postprocess::ResolvePostprocessViewLutIndex(GEditor.viewLUTSdrHandle, GEditor.viewLUTHdrHandle, GContext->enableHDR);
    renderer->ExecuteFrame();
    renderer->EndExecute();
    GEditor.shaderGlobals.ptAccumulatedFrames += PTSamplesPerDispatch(GEditor.shaderGlobals);

    if (cancelRendering || GEditor.shaderGlobals.ptAccumulatedFrames >= targetFrames)
    {
        // Restore spp preview settings
        GEditor.shaderGlobals.ptSamplesPerPixel = GEditor.renderTask.previousSpp;
        GEditor.shaderGlobals.ptDispatchTileSide = GEditor.renderTask.previousSppTile;
        if (!cancelRendering)
            DoRenderReadback(outputs);
        GEditor.state = FERunning;
    }
}

static void DrawInstanceGizmos()
{
    if (!IsSelectedInstanceValid())
        return;
    if (GEditor.selectedLight >= 0)
        return;
    if (!GEditor.viewport.HasRect())
        return;

    auto& pi = SelectedSceneInstance();
    mat4 modelMatrix = translate(mat4(1.0f), vec3(pi.transform.transform)) * mat4_cast(pi.transform.rotation) *
        glm::scale(mat4(1.0f), vec3(pi.transform.scale));

    ImVec2 const displaySize = GEditor.viewport.Size();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    ImGuizmo::BeginFrame();
    ImGuizmo::SetDrawlist(drawList);
    ImGuizmo::SetRect(GEditor.viewport.contentMin.x, GEditor.viewport.contentMin.y, displaySize.x, displaySize.y);

    if (ImGuizmo::Manipulate(&GEditor.camera.view[0][0], &GEditor.camera.proj[0][0],
                             static_cast<ImGuizmo::OPERATION>(GEditor.gizmo.op),
                             static_cast<ImGuizmo::MODE>(GEditor.gizmo.mode), &modelMatrix[0][0], nullptr,
                             GizmoSnapForOperation(static_cast<ImGuizmo::OPERATION>(GEditor.gizmo.op))))
    {
        float3 newTranslation;
        quat newRotation;
        float3 newScale;
        Math::decompose(modelMatrix, newScale, newRotation, newTranslation);
        pi.transform.transform = newTranslation;
        pi.transform.rotation = newRotation;
        pi.transform.scale = newScale;
        CommitSceneToGPU(true);
    }
}

static void DrawLightGizmos()
{
    if (!GEditor.HasScene())
        return;
    auto lights = GEditor.Scene().GetLights();
    if (lights.empty() || !GEditor.viewport.HasRect())
        return;

    if (GEditor.selectedLight < 0 || GEditor.selectedLight >= static_cast<int>(lights.size()))
        return;

    auto& light = lights[GEditor.selectedLight];
    if (light.type == FLightType::Environment)
        return;

    ImVec2 displaySize = GEditor.viewport.Size();
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();

    mat4 modelMatrix = translate(mat4(1.0f), vec3(light.transform.transform))
                     * mat4_cast(light.transform.rotation);

    ImGuizmo::BeginFrame();
    ImGuizmo::SetDrawlist(drawList);
    ImGuizmo::SetRect(GEditor.viewport.contentMin.x, GEditor.viewport.contentMin.y,
                      displaySize.x, displaySize.y);

    ImGuizmo::OPERATION op = static_cast<ImGuizmo::OPERATION>(GEditor.gizmo.op);
    if (op == ImGuizmo::SCALE)
        op = ImGuizmo::TRANSLATE;

    if (ImGuizmo::Manipulate(&GEditor.camera.view[0][0], &GEditor.camera.proj[0][0],
                             op, static_cast<ImGuizmo::MODE>(GEditor.gizmo.mode), &modelMatrix[0][0], nullptr,
                             GizmoSnapForOperation(op)))
    {
        float3 newTranslation;
        quat newRotation;
        float3 newScale;
        Math::decompose(modelMatrix, newScale, newRotation, newTranslation);
        light.transform.transform = newTranslation;
        light.transform.rotation = newRotation;
        UpdateSceneLights();
        GEditor.shaderGlobals.ptAccumulatedFrames = 0;
    }
}
