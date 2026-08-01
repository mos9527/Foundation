#include <cmath>
#include <cfloat>
#include <cstring>
#include <algorithm>
#include <Math/Decompose.hpp>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <Renderer/Postprocess.hpp>
#include <Fonts/PlexSansIcon.h>
#include "PathUtil.hpp"
#include "EditorState.hpp"
#include <Renderer/Mesh.hpp>

using namespace EditorPathUtil;

static void DrawLightGizmos();
static void DrawInstanceGizmos();
static void DrawViewportSelectionContextMenu();

namespace
{
struct DialogResult
{
    SDL_AtomicInt done{};
    String        path;
};

void SDLCALL DialogCallback(void* userdata, const char* const* filelist, int)
{
    auto* result = static_cast<DialogResult*>(userdata);
    if (filelist && *filelist)
        result->path = *filelist;
    SDL_SetAtomicInt(&result->done, 1);
}

bool WaitForDialog(DialogResult& result, String& outPath)
{
    while (SDL_GetAtomicInt(&result.done) == 0)
    {
        SDL_PumpEvents();
        SDL_Delay(1);
    }

    if (result.path.empty())
        return false;
    outPath = result.path;
    return true;
}

bool OpenFile(SDL_DialogFileFilter filter, String& outPath)
{
    DialogResult result{};
    SDL_ShowOpenFileDialog(&DialogCallback, &result, GContext->window,
                           &filter, 1, nullptr, false);
    return WaitForDialog(result, outPath);
}

bool SaveFile(SDL_DialogFileFilter filter, String& outPath)
{
    DialogResult result{};
    SDL_ShowSaveFileDialog(&DialogCallback, &result, GContext->window,
                           &filter, 1, nullptr);
    if (!WaitForDialog(result, outPath))
        return false;

    // Native save dialogs don't reliably append an extension from the filter,
    // so enforce the filter's first extension if the user typed a bare name.
    String ext = LowerExtension(outPath);
    if (!ext.empty())
        ext.erase(0, 1);

    bool matchesFilter = false;
    for (char const* cur = filter.pattern; cur; )
    {
        char const* next = std::strchr(cur, ';');
        size_t len = next ? size_t(next - cur) : std::strlen(cur);
        if (len == ext.size() && SDL_strncasecmp(cur, ext.c_str(), len) == 0)
        {
            matchesFilter = true;
            break;
        }
        cur = next ? next + 1 : nullptr;
    }

    if (!matchesFilter)
    {
        char const* firstExtEnd = std::strchr(filter.pattern, ';');
        auto firstExt = firstExtEnd ? std::string(filter.pattern, firstExtEnd) : std::string(filter.pattern);
        outPath = std::string(outPath.c_str()) + "." + firstExt;
    }
    return true;
}
} // namespace

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

static FInstance& SelectedSceneInstance()
{
    int const idx = SceneInstanceIndexFromId(GEditor.selectedInstance);
    CHECK(idx >= 0);
    return GEditor.Scene().GetInstances()[idx];
}

static int SelectedInstanceIndex()
{
    return SceneInstanceIndexFromId(GEditor.selectedInstance);
}

static bool IsInstanceIndexValid(int index)
{
    return GEditor.HasScene() && GContext->gpuScene &&
           index >= 0 &&
           index < static_cast<int>(GContext->gpuScene->GetInstanceCount()) &&
           index < static_cast<int>(GEditor.Scene().GetInstances().size());
}

static void SelectInstance(uint32_t index, GSInstance const& inst)
{
    ::SelectInstance(GEditor.Scene().GetInstances()[index].id,
                     GEditor.Scene().GetMaterials()[inst.materialIndex].id);
}

static mat4 InstanceTransformMatrix(FTransform const& transform)
{
    return translate(mat4(1.0f), transform.transform) *
           mat4_cast(transform.rotation) *
           scale(mat4(1.0f), transform.scale);
}

static bool GetInstanceWorldBounds(int index, FSerializedBounds& outBounds)
{
    if (!IsInstanceIndexValid(index))
        return false;

    FInstance const& instance = GEditor.Scene().GetInstances()[index];
    FSerializedBounds const* localBounds = InstanceResourceBounds(instance);
    if (!localBounds)
        return false;

    mat4 localToWorld = InstanceTransformMatrix(instance.transform);
    outBounds = FSerializedBounds::Empty();
    for (int x = 0; x < 2; ++x)
    {
        for (int y = 0; y < 2; ++y)
        {
            for (int z = 0; z < 2; ++z)
            {
                float3 p{
                    x ? localBounds->max.x : localBounds->min.x,
                    y ? localBounds->max.y : localBounds->min.y,
                    z ? localBounds->max.z : localBounds->min.z
                };
                float4 world = localToWorld * float4(p, 1.0f);
                float3 worldPoint{world.x, world.y, world.z};
                outBounds += worldPoint;
            }
        }
    }
    return true;
}

static void FrameInstance(int index)
{
    FSerializedBounds bounds{};
    if (!GetInstanceWorldBounds(index, bounds))
        return;

    float3 center = (bounds.min + bounds.max) * 0.5f;
    float radius = length(bounds.max - center);
    float halfFovY = GEditor.camera.fovY * 0.5f;
    float halfFovX = std::atan(std::tan(halfFovY) * std::max(GEditor.camera.aspect, 1e-3f));
    float halfFov = std::max(std::min(halfFovX, halfFovY), 1e-3f);

    GEditor.camera.center = center;
    GEditor.camera.radius = std::max(radius / std::sin(halfFov), 1e-3f);
    GEditor.cameraUpdated = true;
}

static void MoveInstanceToView(int index)
{
    if (!IsInstanceIndexValid(index))
        return;

    GEditor.Scene().GetInstances()[index].transform.transform = GEditor.camera.position;
    CommitSceneToGPU(true);
}

static void AlignInstanceWithView(int index)
{
    if (!IsInstanceIndexValid(index))
        return;

    FTransform& transform = GEditor.Scene().GetInstances()[index].transform;
    transform.transform = GEditor.camera.position;
    transform.rotation = GEditor.camera.rot;
    CommitSceneToGPU(true);
}

static bool IsLightIndexValid(int index)
{
    return GEditor.HasScene() &&
           index >= 0 &&
           index < static_cast<int>(GEditor.Scene().GetLights().size());
}

static bool IsLightTransformable(int index)
{
    return IsLightIndexValid(index) &&
           GEditor.Scene().GetLights()[index].type != FLightType::Environment;
}

static void MoveLightToView(int index)
{
    if (!IsLightTransformable(index))
        return;

    GEditor.Scene().GetLights()[index].transform.transform = GEditor.camera.position;
    UpdateSceneLights();
    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
}

static void AlignLightWithView(int index)
{
    if (!IsLightTransformable(index))
        return;

    FTransform& transform = GEditor.Scene().GetLights()[index].transform;
    transform.transform = GEditor.camera.position;
    transform.rotation = GEditor.camera.rot;
    UpdateSceneLights();
    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
}

static void AlignViewToLight(int index)
{
    if (!IsLightTransformable(index))
        return;

    FTransform const& transform = GEditor.Scene().GetLights()[index].transform;
    GEditor.camera.rot = transform.rotation;
    GEditor.camera.radius = std::max(GEditor.camera.radius, 1e-3f);
    float3 dir = GEditor.camera.rot * float3(0.0f, 0.0f, 1.0f);
    GEditor.camera.center = transform.transform - dir * GEditor.camera.radius;
    GEditor.camera.position = transform.transform;
    GEditor.cameraUpdated = true;
}

static void DeleteLight(int index)
{
    if (!IsLightTransformable(index))
        return;

    auto& lights = GEditor.Scene().mTables.lights;
    FUUID const deletedId = lights[index].id;
    lights.erase(lights.begin() + index);
    GEditor.Scene().RebuildIndex();
    if (GEditor.selectedLight == deletedId)
        ClearSelection();
    UpdateSceneLights();
    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
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
    int const instIdx = SceneInstanceIndexFromId(GEditor.selectedInstance);
    if (instIdx >= 0 && GContext->gpuScene &&
        instIdx < static_cast<int>(GContext->gpuScene->GetInstanceCount()))
        return static_cast<int>(GContext->gpuScene->GetInstance(
            static_cast<uint32_t>(instIdx)).materialIndex);
    return SceneMaterialIndexFromId(GEditor.selectedMaterial);
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
    return GEditor.HasScene() && IsSceneEnvironmentTexture(GEditor.Scene(), textureIndex);
}

static uint32_t SceneTextureIndexFromId(FUUID id)
{
    if (!GEditor.HasScene())
        return kInvalidTexture;
    int const i = GEditor.Scene().TextureIndex(id);
    return i < 0 ? kInvalidTexture : static_cast<uint32_t>(i);
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

static char const* ResolveSceneName(FUUID nameId)
{
    return GEditor.HasScene() ? GEditor.Scene().GetName(nameId) : nullptr;
}

static char const* NameOr(FUUID nameId, char const* fallback)
{
    char const* n = ResolveSceneName(nameId);
    return (n && n[0] != '\0') ? n : fallback;
}

static void ImGuiPushUUID(FUUID const& id)
{
    char buf[40];
    ImGui::PushID(id.Format(buf, sizeof(buf)));
}

static void FormatIndexLabelWithName(char* buf, size_t bufSize, char const* suffix, int index, FUUID nameId)
{
    char const* name = ResolveSceneName(nameId);
    if (name && name[0] != '\0')
        std::snprintf(buf, bufSize, "%s %d (%s)", name, index, suffix);
    else
        std::snprintf(buf, bufSize, "%s %d", suffix, index);
}

static void DrawUUIDRow(FUUID id)
{
    char buf[40];
    ImGui::TextDisabled("UUID: %s", id.Format(buf, sizeof(buf)));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n(click to copy)", buf);
    if (ImGui::IsItemClicked())
        ImGui::SetClipboardText(buf);
}

static void SetUUIDTooltip(FUUID id)
{
    char buf[40];
    ImGui::SetTooltip("UUID: %s", id.Format(buf, sizeof(buf)));
}

static uint32_t SceneTextureToGpuIndex(uint32_t sceneTextureIndex)
{
    if (sceneTextureIndex == kInvalidTexture || !GContext || !GContext->gpuScene)
        return UINT32_MAX;
    if (sceneTextureIndex >= GEditor.resources.textureIDMap.size())
        return UINT32_MAX;
    TextureHandle const& handle = GEditor.resources.textureIDMap[sceneTextureIndex];
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
    FUUID materialId{};
    FUUID FMaterial::*slot{};
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
    state.materialId = kNilUUID;
    state.slot = nullptr;
    state.pendingSelection = kInvalidTexture;
    state.sampler = ImGuiImplFoundationImageSamplerLinear;
}

static void OpenMaterialTexturePickerModal(const char* label, FUUID materialId, FUUID FMaterial::*slot,
                                           ImGui_ImplFoundation_ImageSampler sampler)
{
    auto& state = MaterialTexturePickerModal();
    state.pendingOpen = true;
    std::snprintf(state.label, sizeof(state.label), "%s", label);
    state.materialId = materialId;
    state.slot = slot;
    int const matIdx = SceneMaterialIndexFromId(materialId);
    FUUID const current = (matIdx >= 0) ? GEditor.Scene().GetMaterials()[matIdx].*slot : kNilUUID;
    state.pendingSelection = SceneTextureIndexFromId(current);
    state.sampler = sampler;
}

static void MaterialTexturePickerTileStyleBegin(bool selected)
{
    if (!selected)
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_NavCursor));
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
    ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_NavCursor), 0.0f, 0, 2.0f);
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
            ImGui::Text("#%zu %s  %ux%u", textureIndex, NameOr(texture.name, "unnamed"),
                        texture.GetWidth(), texture.GetHeight());
            char uuidBuf[40];
            ImGui::TextDisabled("UUID: %s", texture.id.Format(uuidBuf, sizeof(uuidBuf)));
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

    if (!state.slot)
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
        FUUID const newId = state.pendingSelection == kInvalidTexture
            ? kNilUUID
            : GEditor.Scene().GetTextures()[state.pendingSelection].id;
        int const matIdx = SceneMaterialIndexFromId(state.materialId);
        if (matIdx >= 0)
        {
            FUUID& slotRef = GEditor.Scene().GetMaterials()[matIdx].*state.slot;
            if (!(slotRef == newId))
            {
                slotRef = newId;
                committed = true;
            }
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
    return MaterialTexturePickerModal().slot != nullptr;
}

static char const* MaterialTexturePropLabel(char* buf, size_t bufSize, char const* name, FUUID textureId)
{
    if (!textureId.IsNil())
        std::snprintf(buf, bufSize, "*%s", name);
    else
        std::snprintf(buf, bufSize, "%s", name);
    return buf;
}

static void DrawMaterialTextureSlot(const char* label, FMaterial& material, FUUID FMaterial::*slot,
                                    ImGui_ImplFoundation_ImageSampler sampler = ImGuiImplFoundationImageSamplerLinear)
{
    ImGui::PushID(label);

    FUUID& sceneTextureId = material.*slot;
    uint32_t const sceneTextureIndex = SceneTextureIndexFromId(sceneTextureId);
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
        OpenMaterialTexturePickerModal(label, material.id, slot, sampler);

    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (sceneTextureId.IsNil())
        ImGui::Text("%s: none", label);
    else if (sceneTextureIndex < GEditor.Scene().GetTextures().size())
    {
        FSerializedTexture const& texture = GEditor.Scene().GetTextures()[sceneTextureIndex];
        char idxBuf[32];
        std::snprintf(idxBuf, sizeof(idxBuf), "#%u", sceneTextureIndex);
        ImGui::Text("%s: %s (%ux%u)", label, NameOr(texture.name, idxBuf), texture.GetWidth(), texture.GetHeight());
        if (ImGui::IsItemHovered())
            SetUUIDTooltip(sceneTextureId);
    }
    else if (preview.textureID == 0)
        ImGui::Text("%s: (unavailable)", label);
    else
        ImGui::Text("%s: #%u", label, sceneTextureIndex);

    ImGui::PopID();
}

static bool DrawHairColorPresets(float4& baseColor)
{
    struct HairColorPreset
    {
        char const* name;
        float3 color;
    };
    // Display sRGB literals; convert to linear on apply.
    static constexpr HairColorPreset presets[] = {
        {"Blond",        {0.94f, 0.87f, 0.52f}},
        {"Dark blond",   {0.63f, 0.43f, 0.24f}},
        {"Medium brown", {0.38f, 0.19f, 0.06f}},
        {"Dark brown",   {0.12f, 0.06f, 0.03f}},
        {"Black",        {0.015f, 0.010f, 0.008f}},
        {"Auburn",       {0.42f, 0.14f, 0.09f}},
        {"Red",          {0.67f, 0.25f, 0.025f}},
        {"Gray",         {0.50f, 0.50f, 0.48f}},
        {"White",        {0.90f, 0.90f, 0.86f}},
    };

    bool changed = false;
    if (ImGui::BeginTable("##HairColorPresets", 3, ImGuiTableFlags_SizingStretchSame))
    {
        for (HairColorPreset const& preset : presets)
        {
            ImGui::TableNextColumn();
            ImGui::PushID(preset.name);

            ImVec4 color{preset.color.x, preset.color.y, preset.color.z, 1.0f};
            ImVec4 hovered{
                std::min(color.x * 1.15f, 1.0f),
                std::min(color.y * 1.15f, 1.0f),
                std::min(color.z * 1.15f, 1.0f),
                1.0f};
            float luminance = 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
            ImGui::PushStyleColor(ImGuiCol_Text, luminance > 0.45f ? ImVec4{0.05f, 0.05f, 0.05f, 1.0f}
                                                                   : ImVec4{0.95f, 0.95f, 0.95f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_Button, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
            if (ImGui::Button(preset.name, ImVec2{-FLT_MIN, 0.0f}))
            {
                float3 const linear = SRGBToLinear(preset.color);
                baseColor.x = linear.x;
                baseColor.y = linear.y;
                baseColor.z = linear.z;
                changed = true;
            }
            ImGui::PopStyleColor(4);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    return changed;
}

static bool DrawPrincipledMaterialPresets(FMaterial& material)
{
    struct MaterialPreset
    {
        char const* name;
        float3 baseColorSRGB;
        float metallic = 0.0f;
        float roughness = 0.5f;
        float transmission = 0.0f;
        float ior = 1.5f;
        float specular = 1.0f;
        float3 specularColor{1.0f};
        float anisotropyStrength = 0.0f;
        float anisotropyRotation = 0.0f;
        float3 sheenColor{0.0f};
        float sheenRoughness = 0.0f;
        float clearcoat = 0.0f;
        float clearcoatRoughness = 0.0f;
        float subsurface = 0.0f;
        float subsurfaceScale = 0.05f;
        float3 subsurfaceColorSRGB{1.0f};
        float3 subsurfaceRadius{1.0f, 0.2f, 0.1f};
    };
    static constexpr MaterialPreset presets[] = {
        {.name = "Matte", .baseColorSRGB = {0.60f, 0.60f, 0.60f}, .roughness = 0.9f},
        {.name = "Plastic", .baseColorSRGB = {0.12f, 0.32f, 0.80f}, .roughness = 0.28f, .ior = 1.46f},
        {.name = "Rubber", .baseColorSRGB = {0.025f, 0.025f, 0.025f}, .roughness = 0.82f, .ior = 1.52f},
        {.name = "Glass", .baseColorSRGB = {0.96f, 0.99f, 1.0f}, .roughness = 0.02f, .transmission = 1.0f},
        {.name = "Gold", .baseColorSRGB = {1.0f, 0.71f, 0.22f}, .metallic = 1.0f, .roughness = 0.2f},
        {.name = "Copper", .baseColorSRGB = {0.95f, 0.45f, 0.22f}, .metallic = 1.0f, .roughness = 0.24f},
        {.name = "Brushed steel",
         .baseColorSRGB = {0.72f, 0.74f, 0.76f},
         .metallic = 1.0f,
         .roughness = 0.34f,
         .anisotropyStrength = 0.8f},
        {.name = "Car paint",
         .baseColorSRGB = {0.55f, 0.015f, 0.02f},
         .roughness = 0.24f,
         .clearcoat = 1.0f,
         .clearcoatRoughness = 0.06f},
        {.name = "Ceramic",
         .baseColorSRGB = {0.92f, 0.90f, 0.84f},
         .roughness = 0.18f,
         .ior = 1.54f,
         .clearcoat = 0.15f,
         .clearcoatRoughness = 0.08f},
        {.name = "Satin",
         .baseColorSRGB = {0.16f, 0.06f, 0.24f},
         .roughness = 0.42f,
         .anisotropyStrength = 0.35f,
         .sheenColor = {0.35f, 0.18f, 0.45f},
         .sheenRoughness = 0.3f},
        {.name = "Wax",
         .baseColorSRGB = {0.90f, 0.58f, 0.28f},
         .roughness = 0.48f,
         .subsurface = 0.7f,
         .subsurfaceScale = 0.12f,
         .subsurfaceColorSRGB = {1.0f, 0.38f, 0.12f},
         .subsurfaceRadius = {1.0f, 0.35f, 0.15f}},
        {.name = "Skin",
         .baseColorSRGB = {0.72f, 0.38f, 0.28f},
         .roughness = 0.52f,
         .subsurface = 1.0f,
         .subsurfaceScale = 0.08f,
         .subsurfaceColorSRGB = {1.0f, 1.0f, 1.0f},
         .subsurfaceRadius = {1.0f, 0.35f, 0.2f}},
    };

    bool changed = false;
    if (ImGui::BeginTable("##PrincipledMaterialPresets", 3, ImGuiTableFlags_SizingStretchSame))
    {
        for (MaterialPreset const& preset : presets)
        {
            ImGui::TableNextColumn();
            ImGui::PushID(preset.name);

            ImVec4 color{preset.baseColorSRGB.x, preset.baseColorSRGB.y, preset.baseColorSRGB.z, 1.0f};
            ImVec4 hovered{
                std::min(color.x * 1.15f, 1.0f),
                std::min(color.y * 1.15f, 1.0f),
                std::min(color.z * 1.15f, 1.0f),
                1.0f};
            float const luminance = 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
            ImGui::PushStyleColor(ImGuiCol_Text, luminance > 0.45f ? ImVec4{0.05f, 0.05f, 0.05f, 1.0f}
                                                                   : ImVec4{0.95f, 0.95f, 0.95f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_Button, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
            bool const clicked = ImGui::Button(preset.name, ImVec2{-FLT_MIN, 0.0f});
            ImGui::PopStyleColor(4);
            ImGui::PopID();
            if (!clicked)
                continue;

            material.baseColorFactor = float4(SRGBToLinear(preset.baseColorSRGB), material.baseColorFactor.w);
            material.metallicFactor = preset.metallic;
            material.roughnessFactor = preset.roughness;
            material.transmissionFactor = preset.transmission;
            material.ior = preset.ior;
            material.specularFactor = preset.specular;
            material.specularColorFactor = preset.specularColor;
            material.anisotropyStrength = preset.anisotropyStrength;
            material.anisotropyRotation = preset.anisotropyRotation;
            material.sheenColorFactor = preset.sheenColor;
            material.sheenRoughnessFactor = preset.sheenRoughness;
            material.clearcoatFactor = preset.clearcoat;
            material.clearcoatRoughnessFactor = preset.clearcoatRoughness;
            material.subsurfaceFactor = preset.subsurface;
            material.subsurfaceScale = preset.subsurfaceScale;
            material.subsurfaceColor = SRGBToLinear(preset.subsurfaceColorSRGB);
            material.subsurfaceRadius = preset.subsurfaceRadius;
            changed = true;
        }
        ImGui::EndTable();
    }
    return changed;
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

        ImGuiID dockBottom;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Down, 0.28f, &dockBottom, &dockCenter);

        ImGuiID dockLeftTop, dockLeftBottom;
        ImGui::DockBuilderSplitNode(dockLeft, ImGuiDir_Up, 0.25f, &dockLeftTop, &dockLeftBottom);
        ImGui::DockBuilderDockWindow("Hierarchy", dockLeftTop);
        ImGui::DockBuilderDockWindow("Inspector", dockLeftBottom);
        ImGui::DockBuilderDockWindow("Material", dockLeftBottom);
        ImGui::DockBuilderDockWindow("Camera", dockRight);
        ImGui::DockBuilderDockWindow("Lighting", dockRight);
        ImGui::DockBuilderDockWindow("Rendering", dockRight);
        ImGui::DockBuilderDockWindow("Profiler", dockRight);
        ImGui::DockBuilderDockWindow("Animation", dockBottom);
        ImGui::DockBuilderFinish(dockspaceID);
    }

    // Main menu bar
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu(PSI_FOLDER_CLOSE " File"))
        {
            if (ImGui::MenuItem(PSI_FOLDER_OPEN " Scene"))
            {
                String path;
                if (OpenFile({"Scene Files", "gltf;glb;fscn"}, path))
                    RequestLoadScene(path);
            }
            if (ImGui::MenuItem(PSI_SUN " HDRI"))
            {
                String path;
                if (OpenFile({"HDR Images", "hdr;hdri"}, path))
                    LoadEnvMap(path);
            }
            ImGui::EndMenu();
        }

        if (GContext->gpuScene && GContext->gpuScene->GetInstanceCount() != 0)
        {
            if (ImGui::BeginMenu(PSI_CAMERA " Render"))
            {
                if (ImGui::MenuItem(PSI_PICTURE " HDR Image"))
                {
                    String path;
                    if (SaveFile({"Radiance HDR", "hdr"}, path))
                    {
                        GEditor.renderTask.outputPath = path;
                        GEditor.renderTask.format = ERenderFormat::HDR;
                        GEditor.renderTask.openRenderPopup = true;
                    }
                }
                if (ImGui::MenuItem(PSI_PICTURE " SDR Image", nullptr, false, !GContext->enableHDR))
                {
                    String path;
                    if (SaveFile({"PNG Image", "png"}, path))
                    {
                        GEditor.renderTask.outputPath = path;
                        GEditor.renderTask.format = ERenderFormat::SDR;
                        GEditor.renderTask.openRenderPopup = true;
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

        if (ImGui::BeginMenu(PSI_EYE_OPEN " View"))
        {
            if (ImGui::MenuItem("Bounding Box", nullptr, &GEditor.gizmo.showBoundingBox))
                GEditor.state = FERunningEnter;
            if (ImGui::MenuItem("Light Gizmos", nullptr, &GEditor.gizmo.showLightGizmos))
                GEditor.state = FERunningEnter;
            if (ImGui::MenuItem("Light BVH Bounds", nullptr, &GEditor.gizmo.showLightBVHBounds))
                GEditor.state = FERunningEnter;
            if (ImGui::MenuItem("ImGuizmo", nullptr, &GEditor.gizmo.showImGuizmo))
                GEditor.state = FERunningEnter;
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
            // Only the progressive path tracer accumulates offline samples; every other mode
            // (realtime PT, raster) exports the next rendered frame.
            const bool progressiveRender = IsProgressive(GEditor.rendererMode);
            const char* formatLabel = GEditor.renderTask.format == ERenderFormat::HDR ? "HDR" : "SDR";
            ImGui::Text("Configure %s %s render:", RendererModeDisplayName(GEditor.rendererMode), formatLabel);
            ImGui::Text("Output: %s", GEditor.renderTask.outputPath.c_str());
            ImGui::Separator();
            if (progressiveRender)
            {
                ImGui::InputInt("Samples / pixel", &GEditor.renderTask.samplePopupInput);
                if (GEditor.renderTask.samplePopupInput < 0)
                    GEditor.renderTask.samplePopupInput = 0;
                ImGui::InputInt("Time limit (s)", &GEditor.renderTask.timePopupInput);
                if (GEditor.renderTask.timePopupInput < 0)
                    GEditor.renderTask.timePopupInput = 0;
                ImGui::TextDisabled("Set to 0 to disable limit");
            }
            else
            {
                ImGui::TextUnformatted("This mode captures the next rendered frame.");
            }
            if (ImGui::Button(PSI_PLAY " Start Render"))
            {
                GEditor.renderTask.targetSamples = progressiveRender ? GEditor.renderTask.samplePopupInput : 1;
                GEditor.renderTask.targetTimeSeconds = progressiveRender ? GEditor.renderTask.timePopupInput : 0;
                GEditor.renderTask.startTime = ImGui::GetTime();
                GEditor.renderTask.renderPaused = false;
                GEditor.renderTask.renderAutoPaused = false;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.renderTask.previousSpp = GEditor.shaderGlobals.ptSamplesPerPixel;
                GEditor.renderTask.previousResolutionScale = GEditor.renderResolutionScale;
                // Go for 1:1, 1spp always during rendering
                GEditor.shaderGlobals.ptSamplesPerPixel = 1;                
                GEditor.renderResolutionScale = 1.0f;
                GEditor.state = FERenderingEnter;                
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

        // Right-aligned renderer-mode toggle: PPT | RTPT | RASTER
        if (GContext->gpuScene && GContext->gpuScene->GetInstanceCount() != 0)
        {
            const float btnW = ImGui::CalcTextSize("######").x + ImGui::GetStyle().FramePadding.x * 2.0f;

            // Renders one mode button (with a full-name tooltip); returns true when it is clicked.
            auto modeButton = [btnW](ERendererMode mode, const char* label, const char* tooltip) -> bool
            {
                const bool active = GEditor.rendererMode == mode;
                ImGui::PushStyleColor(
                    ImGuiCol_Button, ImGui::GetStyle().Colors[active ? ImGuiCol_ButtonActive : ImGuiCol_Button]);
                const bool clicked = ImGui::Button(label, ImVec2(btnW, 0));
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tooltip);
                return clicked;
            };

            // Switches renderer without touching pause state (used by the non-progressive modes).
            auto selectMode = [](ERendererMode mode)
            {
                if (GEditor.rendererMode != mode)
                {
                    GEditor.rendererMode = mode;
                    GEditor.state = FERunningEnter;
                }
            };

            const float totalW = btnW * 3.0f;
            const float avail = ImGui::GetContentRegionAvail().x;
            if (avail > totalW)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - totalW);

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);

            // Three pause states drive the progressive-PT button label:
            //   Running       -> "PPT"
            //   ManualPaused  -> blinking "PAUSED" (sticky; only PT click clears)
            //   AutoPaused    -> blinking "AUTO"   (cleared by any user operation)
            const char* labelPTPause[] = {"PPT", "", "PAUSED", ""};
            const char* labelPTAuto[]  = {"PPT", "", " AUTO ", ""};
            int blink = (SDL_GetTicks() >> 9) & 3;
            const char* ptLabel = labelPTAuto[0];
            if (GEditor.renderTask.renderAutoPaused)
                ptLabel = labelPTAuto[blink];
            else if (GEditor.renderTask.renderPaused)
                ptLabel = labelPTPause[blink];
            if (modeButton(ERendererMode::ProgressivePT, ptLabel, "Progressive Path Tracer"))
            {
                if (GEditor.rendererMode != ERendererMode::ProgressivePT)
                {
                    GEditor.rendererMode = ERendererMode::ProgressivePT;
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

            ImGui::SameLine();
            if (modeButton(ERendererMode::RealtimePT, "RTPT", "Realtime Path Tracer"))
                selectMode(ERendererMode::RealtimePT);

            ImGui::SameLine();
            if (modeButton(ERendererMode::Raster, "RASTER", "Rasterizer"))
                selectMode(ERendererMode::Raster);

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
            auto sceneInstances = GEditor.Scene().GetInstances();
            ImGui::Text("%u instances", instanceCount);
            ImGui::Separator();
            bool deletedInstance = false;
            for (uint32_t i = 0; i < instanceCount && !deletedInstance; i++)
            {
                GSInstance inst = gpu->GetInstance(i);
                if (i >= sceneInstances.size())
                    continue;
                FUUID const& instanceId = sceneInstances[i].id;
                ImGuiPushUUID(instanceId);

                char label[128];
                FormatIndexLabelWithName(label, sizeof(label), InstanceTypeName(inst.type), static_cast<int>(i),
                                         sceneInstances[i].name);
                bool selected = (GEditor.selectedInstance == instanceId);                
                if (ImGui::Selectable(label, selected))
                {
                    SelectInstance(i, inst);
                }
                if (ImGui::IsItemHovered())
                    SetUUIDTooltip(instanceId);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    FrameInstance(static_cast<int>(i));
                if (ImGui::BeginPopupContextItem())
                {
                    SelectInstance(i, inst);
                    FSerializedBounds bounds{};
                    bool const hasBounds = GetInstanceWorldBounds(static_cast<int>(i), bounds);
                    if (ImGui::MenuItem("Frame Selected", nullptr, false, hasBounds))
                        FrameInstance(static_cast<int>(i));
                    if (ImGui::MenuItem("Move to View"))
                        MoveInstanceToView(static_cast<int>(i));
                    if (ImGui::MenuItem("Align with View"))
                        AlignInstanceWithView(static_cast<int>(i));
                    ImGui::Separator();
                    if (ImGui::MenuItem(PSI_TRASH " Delete"))
                    {
                        DeleteSelectedInstance();
                        deletedInstance = true;
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
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
            GEditor.selectedMaterial = GEditor.Scene().GetMaterials()[materialIndex].id;

            auto& material = GEditor.Scene().GetMaterials()[materialIndex];
            ImGuiPushUUID(material.id);

            char materialLabel[128];
            FormatIndexLabelWithName(materialLabel, sizeof(materialLabel), "Material", materialIndex, material.name);
            ImGui::TextDisabled("%s", materialLabel);
            DrawUUIDRow(material.id);
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

                    char propLabel[64];
                    ImGui::SeparatorText("Common");
                    changed |= ImLinearColorEdit4(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Base Color", material.baseColorTexture),
                                                 material.baseColorFactor);
                    changed |= ImHDRColorEdit(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Emissive", material.emissiveTexture),
                                              reinterpret_cast<float3&>(material.emissiveFactor), material.emissiveFactor.w /* otherwise unused */);
                    changed |= ImGui::DragFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Normal Scale", material.normalTexture),
                                                &material.normalScale, 0.01f, -8.0f, 8.0f, "%.3f");
                    changed |= ImGui::SliderFloat("IOR", &material.ior, 1.0f, 3.0f, "%.3f");
                    changed |= ImGui::DragFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Anisotropy Rotation", material.anisotropyTexture),
                                                &material.anisotropyRotation, 0.01f, -FLT_MAX, FLT_MAX, "%.3f rad");

                    if (material.shaderBlockID == FMaterialShaderBlock::Principled)
                    {
                        ImGui::SeparatorText("Principled");
                        changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Metallic", material.metallicRoughnessTexture),
                                                      &material.metallicFactor, 0.0f, 1.0f, "%.3f");
                        changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Roughness", material.metallicRoughnessTexture),
                                                      &material.roughnessFactor, 0.0f, 1.0f, "%.3f");
                        changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Transmission", material.transmissionTexture),
                                                      &material.transmissionFactor, 0.0f, 1.0f, "%.3f");
                        changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Specular", material.specularTexture),
                                                      &material.specularFactor, 0.0f, 1.0f, "%.3f");
                        changed |= ImGui::DragFloat3(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Specular Color", material.specularColorTexture),
                                                     &material.specularColorFactor.x, 0.01f, 0.0f, FLT_MAX, "%.3f");
                        changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Anisotropy Strength", material.anisotropyTexture),
                                                      &material.anisotropyStrength, 0.0f, 1.0f, "%.3f");

                        ImGui::SeparatorText("Sheen");
                        changed |= ImLinearColorEdit3(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Sheen Color", material.sheenColorTexture),
                                                      material.sheenColorFactor);
                        changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Sheen Roughness", material.sheenRoughnessTexture),
                                                      &material.sheenRoughnessFactor, 0.0f, 1.0f, "%.3f");

                        ImGui::SeparatorText("Clearcoat");
                        changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Clearcoat Weight", material.clearcoatTexture),
                                                      &material.clearcoatFactor, 0.0f, 1.0f, "%.3f");
                        changed |= ImGui::SliderFloat(MaterialTexturePropLabel(propLabel, sizeof(propLabel), "Clearcoat Roughness", material.clearcoatRoughnessTexture),
                                                      &material.clearcoatRoughnessFactor, 0.0f, 1.0f, "%.3f");

                        ImGui::SeparatorText("Subsurface");
                        changed |= ImGui::SliderFloat("Weight", &material.subsurfaceFactor, 0.0f, 1.0f, "%.3f");
                        changed |= ImLinearColorEdit3("Color", material.subsurfaceColor);
                        changed |= ImGui::DragFloat3("Radius", &material.subsurfaceRadius.x, 0.001f, 0.0f, FLT_MAX, "%.4f");
                        changed |= ImGui::SliderFloat("Scale", &material.subsurfaceScale, 0.0f, 1.0f, "%.4f");

                        ImGui::SeparatorText("Material Presets");
                        changed |= DrawPrincipledMaterialPresets(material);
                    }
                    else
                    {
                        ImGui::SeparatorText("Hair Color Presets");
                        changed |= DrawHairColorPresets(material.baseColorFactor);
                        ImGui::SeparatorText("Hair");
                        changed |= ImGui::SliderFloat("Beta M", &material.hairBetaM, 0.0f, 1.0f, "%.3f");
                        changed |= ImGui::SliderFloat("Beta N", &material.hairBetaN, 0.0f, 1.0f, "%.3f");
                        changed |= ImGui::DragFloat("Alpha", &material.hairAlpha, 0.1f, -90.0f, 90.0f, "%.2f deg");
                    }

                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem(PSI_PICTURE " Textures"))
                {
                    DrawMaterialTextureSlot("Base Color", material, &FMaterial::baseColorTexture);
                    DrawMaterialTextureSlot("Emissive", material, &FMaterial::emissiveTexture);
                    DrawMaterialTextureSlot("Normal", material, &FMaterial::normalTexture, ImGuiImplFoundationImageSamplerNearest);
                    DrawMaterialTextureSlot("Anisotropy", material, &FMaterial::anisotropyTexture);
                    if (material.shaderBlockID == FMaterialShaderBlock::Principled)
                    {
                        DrawMaterialTextureSlot("Metallic/Roughness", material, &FMaterial::metallicRoughnessTexture);
                        DrawMaterialTextureSlot("Transmission", material, &FMaterial::transmissionTexture);
                        DrawMaterialTextureSlot("Specular", material, &FMaterial::specularTexture);
                        DrawMaterialTextureSlot("Specular Color", material, &FMaterial::specularColorTexture);
                        DrawMaterialTextureSlot("Sheen Color", material, &FMaterial::sheenColorTexture);
                        DrawMaterialTextureSlot("Sheen Roughness", material, &FMaterial::sheenRoughnessTexture);
                        DrawMaterialTextureSlot("Clearcoat", material, &FMaterial::clearcoatTexture);
                        DrawMaterialTextureSlot("Clearcoat Roughness", material, &FMaterial::clearcoatRoughnessTexture);
                    }

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
            ImGui::PopID();
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
            int const idx = SelectedInstanceIndex();
            auto& pi = SelectedSceneInstance();
            ImGuiPushUUID(pi.id);

            GSInstance inst = GContext->gpuScene->GetInstance(static_cast<uint32_t>(idx));
            char instHeader[128];
            FormatIndexLabelWithName(instHeader, sizeof(instHeader), InstanceTypeName(inst.type), idx, pi.name);
            ImGui::TextUnformatted(instHeader);
            DrawUUIDRow(pi.id);
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
                inst = GContext->gpuScene->GetInstance(static_cast<uint32_t>(idx));
            }
            ImGui::Separator();
            ImGui::Text("Type: %s", InstanceTypeName(inst.type));
            ImGui::Text("Resource Offset: %u", inst.resourceOffset);
            ImGui::Text("Material Index: %u", inst.materialIndex);
            ImGui::Text("Resource Index: %u", inst.resourceIndex);
            ImGui::PopID();
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
    if (GEditor.scrollSelectedLightToTop && !GEditor.selectedLight.IsNil())
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
        static const char* kLightTypeNames[] = {"Environment", "Directional", "Point", "Spot", "Disk", "Rect"};
        static constexpr int kLightTypeCount = 6;

        // ---- Scene Lights ----
            GEditor.Scene().EnsureEnvironmentLight();
            auto& lights = GEditor.Scene().mTables.lights;
            uint32_t lightCapacity = GContext->gpuScene ? GContext->gpuScene->GetLightCapacity() : 0u;
            bool canAddLight = GContext->gpuScene && lights.size() < lightCapacity;
            if (!canAddLight)
                ImGui::BeginDisabled();
            if (ImModalButton(canAddLight ? PSI_PLUS_SIGN " Add Light" : "(Lights Full)"))
            {
                FLight light{};
                light.id = FUUID::Generate();
                light.type = FLightType::Point;
                light.transform.transform = GEditor.camera.position;
                light.transform.rotation = GEditor.camera.rot;
                light.color = float3{1.0f, 0.92f, 0.78f};
                light.power = 10.0f;
                size_t const insertIndex = !lights.empty() && lights.front().type == FLightType::Environment ? 1u : 0u;
                lights.insert(lights.begin() + static_cast<std::ptrdiff_t>(insertIndex), light);
                GEditor.Scene().RebuildIndex();
                SelectLight(lights[insertIndex].id);
                GEditor.scrollSelectedLightToTop = true;
                GEditor.selectedLightHighlightStart = static_cast<float>(ImGui::GetTime());
                anyChanged = true;
            }
            if (!canAddLight)
                ImGui::EndDisabled();
            ImGui::Separator();
            static constexpr float kLightHighlightDuration = 1.2f;

            auto selectLight = [&](int i)
            {
                SelectLight(lights[i].id);
                GEditor.scrollSelectedLightToTop = true;
                GEditor.selectedLightHighlightStart = static_cast<float>(ImGui::GetTime());
            };
            auto removeLight = [&](int i)
            {
                DeleteLight(i);
            };
            auto moveLightToView = [&](int i)
            {
                MoveLightToView(i);
            };
            auto alignLightWithView = [&](int i)
            {
                AlignLightWithView(i);
            };
            auto alignViewToLight = [&](int i)
            {
                AlignViewToLight(i);
            };

            int contextLight = -1;
            for (int i = 0; i < static_cast<int>(lights.size()); i++)
            {
                auto& light = lights[i];
                bool const isEnvironment = light.type == FLightType::Environment;
                ImGuiPushUUID(light.id);

                char label[128];
                std::snprintf(label, sizeof(label), PSI_BOLT " Light %d (%s)", i, LightTypeName(light.type));
                char const* lightName = ResolveSceneName(light.name);
                if (lightName && lightName[0] != '\0')
                {
                    size_t const len = std::strlen(label);
                    std::snprintf(label + len, sizeof(label) - len, " (%s)", lightName);
                }
                bool isLightSelected = (GEditor.selectedLight == light.id);

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

                if (ImGui::Selectable(label, isLightSelected))
                    selectLight(i);
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    selectLight(i);
                    alignViewToLight(i);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                {
                    selectLight(i);
                    contextLight = i;
                }
                if (pushedHeaderTextColor)
                    ImGui::PopStyleColor();

                if (isLightSelected && GEditor.scrollSelectedLightToTop)
                {
                    ImGui::SetScrollHereY(0.0f);
                    GEditor.scrollSelectedLightToTop = false;
                }
                ImGui::PopID();
            }
            if (contextLight >= 0)
                ImGui::OpenPopup("LightContextMenu");
            if (ImGui::BeginPopup("LightContextMenu"))
            {
                int const i = SceneLightIndexFromId(GEditor.selectedLight);
                bool const canRemove = i >= 0 &&
                    i < static_cast<int>(lights.size()) &&
                    lights[i].type != FLightType::Environment;
                bool const canTransform = canRemove;
                if (ImGui::MenuItem("Move to View", nullptr, false, canTransform))
                    moveLightToView(i);
                if (ImGui::MenuItem("Align with View", nullptr, false, canTransform))
                    alignLightWithView(i);
                if (ImGui::MenuItem("Align View To Selected", nullptr, false, canTransform))
                    alignViewToLight(i);
                ImGui::Separator();
                if (ImGui::MenuItem(PSI_TRASH " Remove Light", nullptr, false, canRemove))
                    removeLight(i);
                if (!canTransform && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Environment light is required");
                ImGui::EndPopup();
            }

            int const selectedLightIdx = SceneLightIndexFromId(GEditor.selectedLight);
            if (selectedLightIdx >= 0 && selectedLightIdx < static_cast<int>(lights.size()))
            {
                int const i = selectedLightIdx;
                auto& light = lights[i];
                bool const isEnvironment = light.type == FLightType::Environment;
                bool lightChanged = false;
                ImGui::Separator();
                ImGuiPushUUID(light.id);
                char lightHeader[128];
                std::snprintf(lightHeader, sizeof(lightHeader), "%s %d (%s)", PSI_BOLT " Light", i,
                              LightTypeName(light.type));
                char const* lightName = ResolveSceneName(light.name);
                if (lightName && lightName[0] != '\0')
                {
                    size_t const len = std::strlen(lightHeader);
                    std::snprintf(lightHeader + len, sizeof(lightHeader) - len, " (%s)", lightName);
                }
                ImGui::TextUnformatted(lightHeader);
                DrawUUIDRow(light.id);

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
                        if (typeInt != 0) // No more than 1 env light, which is always #0
                        {
                            light.type = static_cast<FLightType>(typeInt);
                            lightChanged = true;
                        }
                    }
                }

                // Color + Power
                lightChanged |= ImHDRColorEdit(isEnvironment ? "Ambient" : "Color", light.color, light.power);

                if (isEnvironment)
                {
                    ImGui::Separator();
                    bool hasEnv = GContext->gpuScene && GContext->gpuScene->HasEnvironmentMap();
                    ImGui::Text(hasEnv ? PSI_OK " HDRI Loaded" : PSI_WARNING_SIGN " No HDRI");
                    if (hasEnv)
                    {
                        DrawTexturePreview("HDRI", GContext->gpuScene->mEnvMapIndex.index);
                        if (!light.environmentTexture.IsNil())
                        {
                            uint32_t const envTexIdx = SceneTextureIndexFromId(light.environmentTexture);
                            if (envTexIdx != kInvalidTexture)
                            {
                                char const* envTexName = NameOr(GEditor.Scene().GetTextures()[envTexIdx].name, nullptr);
                                if (envTexName)
                                    ImGui::TextDisabled("Source: %s", envTexName);
                            }
                            DrawUUIDRow(light.environmentTexture);
                        }
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
                if (light.type == FLightType::Point || light.type == FLightType::Spot)
                {
                    lightChanged |= ImGui::DragFloat("Radius", &light.radius, 0.01f, 0.0f, 100.0f, "%.3f");
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

                if (light.type != FLightType::Environment)
                    lightChanged |= ImGui::Checkbox("Shadows", &light.useShadow);

                // Two-sided toggle for area lights
                if (light.type == FLightType::Disk || light.type == FLightType::Rect)
                {
                    lightChanged |= ImGui::Checkbox("Two-Sided", &light.twoSided);
                    ImGui::SameLine();
                    lightChanged |= ImGui::Checkbox("Normalize", &light.normalize);
                }

                if (lightChanged)
                    anyChanged = true;
                ImGui::PopID();
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

static void FAnimationPanel()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Animation"))
    {
        if (!GEditor.animation || !GEditor.animation->HasSkinning())
        {
            ImGui::TextDisabled("No skinned animation");
        }
        else
        {
            auto playbacks = GEditor.animation->GetPlaybacks();
            auto skeletons = GEditor.Scene().GetSkeletons();
            auto clips = GEditor.Scene().GetClips();
            static int selectedSkeleton = 0;
            selectedSkeleton = std::clamp(selectedSkeleton, 0, static_cast<int>(playbacks.size()) - 1);
            char const* skeletonName = GEditor.Scene().GetName(skeletons[selectedSkeleton].id);
            String skeletonFallback = Format("Skeleton {}", selectedSkeleton);
            if (ImGui::BeginCombo("Skeleton", skeletonName ? skeletonName : skeletonFallback.c_str()))
            {
                for (int i = 0; i < static_cast<int>(playbacks.size()); ++i)
                {
                    char const* name = GEditor.Scene().GetName(skeletons[i].id);
                    String fallback = Format("Skeleton {}", i);
                    if (ImGui::Selectable(name ? name : fallback.c_str(), selectedSkeleton == i))
                        selectedSkeleton = i;
                }
                ImGui::EndCombo();
            }

            FAnimationPlayback& playback = playbacks[selectedSkeleton];
            Span<const uint32_t> skeletonClips =
                GEditor.animation->GetSkeletonClips(static_cast<uint32_t>(selectedSkeleton));
            char const* clipName = playback.clipIndex < clips.size()
                ? GEditor.Scene().GetName(clips[playback.clipIndex].name)
                : nullptr;
            if (ImGui::BeginCombo("Clip", clipName ? clipName : "Rest pose"))
            {
                for (uint32_t clipIndex : skeletonClips)
                {
                    char const* name = GEditor.Scene().GetName(clips[clipIndex].name);
                    String fallback = Format("Clip {}", clipIndex);
                    if (ImGui::Selectable(name ? name : fallback.c_str(), playback.clipIndex == clipIndex))
                    {
                        playback.clipIndex = clipIndex;
                        playback.time = 0.0f;
                        playback.dirty = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::Checkbox("Play", &playback.playing);
            ImGui::SameLine();
            ImGui::Checkbox("Loop", &playback.loop);
            ImGui::DragFloat("Speed", &playback.speed, 0.05f, -8.0f, 8.0f, "%.2fx");
            float duration = playback.clipIndex < clips.size() ? clips[playback.clipIndex].duration : 0.0f;
            if (ImGui::SliderFloat("Time", &playback.time, 0.0f, std::max(duration, 0.0f), "%.3f s"))
                playback.dirty = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
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
        ImGui::TextUnformatted("Mouse Left: Rotate | Mouse Right: Pan | Mouse Wheel: Zoom | WASD: Move | Shift: Fast | Space: Toggle Orbit/Free");
        ImGui::Separator();
        GEditor.cameraUpdated |= ImGui::SliderFloat3("Cam Center", &GEditor.camera.center.x, -50.0f, 50.0f);
        GEditor.cameraUpdated |= ImGui::SliderFloat("Cam Radius", &GEditor.camera.radius, 0.0f, 100.0f);
        {
            // YXZ intrinsic: rotateY(yaw) * rotateX(pitch) * rotateZ(roll)
            float sinP = 2.0f * (GEditor.camera.rot.w * GEditor.camera.rot.x -
                                 GEditor.camera.rot.y * GEditor.camera.rot.z);
            sinP = std::clamp(sinP, -1.0f, 1.0f);
            float pitch = degrees(std::asin(sinP));

            float sinY = 2.0f * (GEditor.camera.rot.w * GEditor.camera.rot.y +
                                 GEditor.camera.rot.x * GEditor.camera.rot.z);
            float cosY = 1.0f - 2.0f * (GEditor.camera.rot.x * GEditor.camera.rot.x +
                                          GEditor.camera.rot.y * GEditor.camera.rot.y);
            float yaw = degrees(std::atan2(sinY, cosY));

            float sinR = 2.0f * (GEditor.camera.rot.w * GEditor.camera.rot.z +
                                 GEditor.camera.rot.x * GEditor.camera.rot.y);
            float cosR = 1.0f - 2.0f * (GEditor.camera.rot.y * GEditor.camera.rot.y +
                                          GEditor.camera.rot.z * GEditor.camera.rot.z);
            float roll = degrees(std::atan2(sinR, cosR));

            bool rotChanged = false;
            rotChanged |= ImGui::SliderFloat("Pitch", &pitch, -90.0f, 90.0f, "%.1f deg");
            rotChanged |= ImGui::SliderFloat("Yaw", &yaw, -180.0f, 180.0f, "%.1f deg");
            rotChanged |= ImGui::SliderFloat("Roll", &roll, -180.0f, 180.0f, "%.1f deg");
            if (rotChanged)
            {
                quat yawQ = angleAxis(radians(yaw), vec3(0, 1, 0));
                quat pitchQ = angleAxis(radians(pitch), vec3(1, 0, 0));
                quat rollQ = angleAxis(radians(roll), vec3(0, 0, 1));
                GEditor.camera.rot = normalize(yawQ * pitchQ * rollQ);
                GEditor.cameraUpdated = true;
            }
        }
        const char* projectionItems[] = {"Perspective", "Panoramic (Equirectangular)"};
        int cameraProjection = static_cast<int>(GEditor.shaderGlobals.cameraProjection);
        if (ImGui::Combo("Projection", &cameraProjection, projectionItems, IM_ARRAYSIZE(projectionItems)))
        {
            GEditor.shaderGlobals.cameraProjection = to_integer(static_cast<CameraProjection>(cameraProjection));
            GEditor.cameraUpdated = true;
        }
        bool perspectiveCamera = GEditor.shaderGlobals.cameraProjection == to_integer(CameraProjection::Perspective);
        if (perspectiveCamera)
            GEditor.cameraUpdated |= ImGui::SliderAngle("Cam FOV Y", &GEditor.camera.fovY);
        else
            ImGui::TextDisabled("Renders a 360x180 equirectangular view.");
        GEditor.cameraUpdated |= ImGui::SliderFloat("Z Near", &GEditor.camera.zNear, 0.001f, 10.0f, "%.4f",
                                                    ImGuiSliderFlags_Logarithmic);
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
                    GContext->device->QueryBudget(RHIDeviceHeapType::Local, used, budget);
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
                static float gpuTimingMS = 0.0f;
                static int lanes = 0;
                float frametimeAvg = frametime.mean * 1e-6f;
                ImGui::Text("GPU Profiler: CPU %.2f ms (%.1f FPS) / GPU %.2f ms", frametimeAvg * 1e3f, 1 / frametimeAvg, gpuTimingMS);
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
                        if (!pass.used)
                            continue;
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
                    lanes = ImProfilerAssignLanes(samples, frameScratch);
                    gpuTimingMS = (samples.back().endTick - samples.front().startTick) * 1e-6;
                    gpuTimingMS *= gpuTimingRes;
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
    FAnimationPanel();
    FLightingPanel();
    DrawInstanceGizmos();
    DrawViewportSelectionContextMenu();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Rendering"))
    {
        bool changed = false;
        ImGui::SeparatorText(PSI_EYE_OPEN " Display");
        {
            static float sPendingScale = GEditor.renderResolutionScale;
            RHIExtent2D displayExtent = GEditor.viewport.renderExtent;
            uint32_t internalW = std::max(16u, static_cast<uint32_t>(displayExtent.x * sPendingScale));
            uint32_t internalH = std::max(16u, static_cast<uint32_t>(displayExtent.y * sPendingScale));
            ImGui::Text("Display: %ux%u, Internal: %ux%u", displayExtent.x, displayExtent.y, internalW, internalH);
            ImGui::SliderFloat("Render Scale", &sPendingScale, 0.25f, 1.0f, "%.2fx");
            sPendingScale = std::clamp(sPendingScale, 0.25f, 1.0f);
            // Commit scale + rebuild render graph only when the user releases the slider
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                GEditor.renderResolutionScale = sPendingScale;
                GEditor.state = FERunningEnter;
                if (IsPathTracer(GEditor.rendererMode))
                    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
        }
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
                    String path;
                    if (OpenFile({"DDS LUT", "dds"}, path))
                    {
                        externalPath = path;
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
            if (IsPathTracer(GEditor.rendererMode))
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
        }
        {
            float anisoLevel = GEditor.rendererConfig.textureAnisoLevel;
            ImGui::BeginDisabled(!GEditor.rendererConfig.textureAnisoEnable);
            if (ImGui::SliderFloat("Anisotropy Level", &anisoLevel, 1.0f, 16.0f, "%.0f"))
            {
                GEditor.rendererConfig.textureAnisoLevel = anisoLevel;
                changed = true;
                if (IsPathTracer(GEditor.rendererMode))
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
                if (IsPathTracer(GEditor.rendererMode))
                    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
        }
        // Progressive-only telemetry & sample auto-pause.
        if (IsProgressive(GEditor.rendererMode))
        {
            ImGui::SeparatorText(PSI_SIGNAL " Stats");
            // Throughput: measure delta on the *frame* (dispatch) counter every ~250ms,
            // then derive samples/sec from frames/sec via the spp multiplier.
            //   frames  = ptAccumulatedFrames / ptSamplesPerPixel      (dispatch count)
            //   samples = ptAccumulatedFrames
            // i.e. one dispatch advances `spp` pixel-samples.
            static double   sLastSampleTime  = ImGui::GetTime();
            static uint32_t sLastFrameCount  = 0u;
            static float    sFramesPerSec    = 0.0f;
            uint32_t samplesPerDispatch = GEditor.shaderGlobals.ptSamplesPerPixel;
            uint32_t frameCount         = GEditor.shaderGlobals.ptAccumulatedFrames / samplesPerDispatch;
            uint32_t completedSamples   = GEditor.shaderGlobals.ptAccumulatedFrames;
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
            ImGui::SeparatorText(PSI_BOLT " Path Tracer");
            float samplesPerSec = sFramesPerSec * static_cast<float>(samplesPerDispatch);
            ImGui::Text("samples: %u (%.2f sps), frames: %u (%.1f fps)",
                        completedSamples, samplesPerSec, frameCount, sFramesPerSec);
            if (ImModalButton(PSI_ADJUST " 1sp", 0, 3))
            {
                GEditor.renderTask.autoPauseSampleLimit = 1;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            };
            if (ImModalButton(PSI_ADJUST " 8sp", 1, 3))
            {
                GEditor.renderTask.autoPauseSampleLimit = 8;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            };
            if (ImModalButton(PSI_ADJUST " NoLimit", 2, 3))
            {
                GEditor.renderTask.autoPauseSampleLimit = 0;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            };
            int limit = GEditor.renderTask.autoPauseSampleLimit;
            if (ImGui::SliderInt("Samples", &limit, 0, 4096, limit > 0 ? "%d" : "Off", ImGuiSliderFlags_Logarithmic))
            {
                GEditor.renderTask.autoPauseSampleLimit = std::max(0, limit);
                if (GEditor.renderTask.renderAutoPaused &&
                    GEditor.renderTask.autoPauseSampleLimit > static_cast<int>(completedSamples))
                {
                    GEditor.renderTask.renderAutoPaused = false;
                    GEditor.renderTask.renderPaused = false;
                }
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
        }
        // Controls shared by every path tracer (progressive + realtime).
        if (IsPathTracer(GEditor.rendererMode))
        {
            if (!IsProgressive(GEditor.rendererMode))
                ImGui::SeparatorText(PSI_BOLT " Path Tracer");
            if (ImModalButton(PSI_CODE " Direct", 0, 4))
            {
                GEditor.shaderGlobals.ptMaxBounces = 0;
                GEditor.shaderGlobals.ptFireflyClamp = GContext->rendererSettings.energyClampOverride; // Default 2.0
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            if (ImModalButton(PSI_BOLT " Fast", 1, 4))
            {
                GEditor.shaderGlobals.ptMaxBounces = 4;
                GEditor.shaderGlobals.ptFireflyClamp = GContext->rendererSettings.energyClampOverride; // Default 2.0
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            if (ImModalButton(PSI_FIRE " Full", 2, 4))
            {
                GEditor.shaderGlobals.ptMaxBounces = 32;
                GEditor.shaderGlobals.ptFireflyClamp = GContext->rendererSettings.energyClampOverride; // Default 2.0
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            if (ImModalButton(PSI_BEAKER " Über", 3, 4))
            {
                GEditor.shaderGlobals.ptMaxBounces = 100;
                GEditor.shaderGlobals.ptFireflyClamp = 100.0f;
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::SeparatorText(PSI_RANDOM " Ray Bounce");
            if (ImGui::SliderInt("Max Bounces", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBounces), 0, 100,
                                 "%d", ImGuiSliderFlags_Logarithmic))
            {
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
            if (GEditor.rendererMode == ERendererMode::RealtimePT &&
                ImGui::Checkbox("SHARC", &GEditor.rendererConfig.ptSharc))
            {
                if (!GEditor.rendererConfig.ptSharc)
                    GEditor.rendererConfig.viewFlags &=
                        ~(ViewFlagsBits::SHARCGrid | ViewFlagsBits::SHARCOccupancy |
                          ViewFlagsBits::SHARCRadiance);
                GEditor.state = FERunningEnter;
            }
            if (ImGui::Checkbox("Force Texture LOD 0", &GEditor.rendererConfig.forceTextureLOD0))
            {
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            if (ImGui::Checkbox("Energy Compensation", &GEditor.rendererConfig.energyCompensation))
            {
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            if (ImGui::Checkbox("Primary Light Visibility", &GEditor.rendererConfig.ptPrimaryLightVisibility))
            {
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
            ImGui::SeparatorText(PSI_RANDOM " Sampling");
            const char* lightSamplerItems[] = {"Light BVH", "Uniform (Reference)"};
            int lightSampler = static_cast<int>(GEditor.rendererConfig.lightSamplerMode);
            if (ImGui::Combo("Light Sampler", &lightSampler, lightSamplerItems, 2))
            {
                GEditor.rendererConfig.lightSamplerMode = static_cast<LightSampler>(lightSampler);
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            ImGui::SliderFloat("Max Energy", &GEditor.shaderGlobals.ptFireflyClamp, 1.0f, 100.0f, "%.1f");
            if (ImGui::SliderFloat("Adaptive Threshold", &GEditor.shaderGlobals.adaptiveThreshold, 0.0f, 1.0f, "%.4f"))
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            if (ImGui::SliderInt("Adaptive Min Samples", reinterpret_cast<int*>(&GEditor.shaderGlobals.adaptiveMinSamples), 1, 256))
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            int spp = static_cast<int>(GEditor.shaderGlobals.ptSamplesPerPixel);
            if (ImGui::SliderInt("SPP", &spp, 1, 64, "%d", ImGuiSliderFlags_Logarithmic))
            {
                if (spp != static_cast<int>(GEditor.shaderGlobals.ptSamplesPerPixel))
                {
                    GEditor.shaderGlobals.ptSamplesPerPixel = static_cast<uint32_t>(std::max(1, spp));
                    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
                }
            }
            const char* samplerItems[] = {"PCG (Independent)", "Sobol (Quasi-Monte Carlo)"};
            int ptSampler = static_cast<int>(GEditor.rendererConfig.ptSampler);
            if (ImGui::Combo("Sampler", &ptSampler, samplerItems, 2))
            {
                GEditor.rendererConfig.ptSampler = static_cast<PTSampler>(ptSampler);
                GEditor.shaderGlobals.ptAccumulatedFrames = 0;                
                GEditor.state = FERunningEnter;
            }
        }
        if (IsRaster(GEditor.rendererMode))
        {
            ImGui::SeparatorText(PSI_EYE_OPEN " Rasterizer");
            static float lodLogThreshold = 5;
            ImGui::SliderFloat("LOD ", &lodLogThreshold, 0, 8);
            GEditor.shaderGlobals.lodThreshold = std::pow(10.0f, -lodLogThreshold);
            ImGui::SeparatorText(PSI_DASHBOARD " Performance");
            changed |= ImGui::Checkbox("Force Texture LOD 0", &GEditor.rendererConfig.forceTextureLOD0);
            {
                const char* names[] = {"Overdraw", "Meshlet", "Matcap"};
                const ViewFlagsBits values[] = {ViewFlagsBits::Overdraw, ViewFlagsBits::Meshlet, ViewFlagsBits::Matcap};
                ImGui::SeparatorText(PSI_EYE_OPEN " Raster Debug View");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, names, values, true /* solo */);
            }
            {
                const char* items[] = {"RT Shadows"};
                const ViewFlagsBits values[] = {ViewFlagsBits::EnableRasterRTShadows};
                ImGui::SeparatorText(PSI_COG " Options");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values);
                ImGui::BeginDisabled((GEditor.rendererConfig.viewFlags & ViewFlagsBits::EnableRasterRTShadows) == 0u);
                ImGui::SliderFloat("RT Shadow Bias", &GEditor.shaderGlobals.rasterRTShadowBias,
                                   0.0f, 0.05f, "%.4f");
                ImGui::EndDisabled();
            }
            {
                ImGui::SeparatorText(PSI_COG " Raster Effects");
                if (ImGui::CollapsingHeader("GTAO", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::PushID("GTAO");
                    changed |= ImGui::Checkbox("Enable GTAO", &GEditor.rasterGTAO);
                    ImGui::BeginDisabled(!GEditor.rasterGTAO);
                    GTAOConfig& gtao = GEditor.rasterGTAOConfig;
                    ImGui::SliderFloat("Radius Pixels", &gtao.radiusPixels, 4.0f, 96.0f, "%.0f");
                    ImGui::SliderFloat("Radius World", &gtao.radiusWorld, 0.1f, 8.0f, "%.2f");
                    ImGui::SliderFloat("Intensity", &gtao.intensity, 0.0f, 6.0f, "%.2f");
                    ImGui::SliderFloat("Bias", &gtao.bias, 0.0f, 0.25f, "%.3f");
                    int directions = static_cast<int>(gtao.directionCount);
                    if (ImGui::SliderInt("Directions", &directions, 1, 8))
                        gtao.directionCount = static_cast<uint32_t>(directions);
                    int steps = static_cast<int>(gtao.stepCount);
                    if (ImGui::SliderInt("Steps", &steps, 1, 16))
                        gtao.stepCount = static_cast<uint32_t>(steps);
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }                
            }
            if ((GEditor.rendererConfig.viewFlags & ViewFlagsBits::Matcap) != 0u)
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
                        String path;
                        if (OpenFile({"Matcap Image", "png;jpg;jpeg;tga;bmp;dds"}, path))
                        {
                            GEditor.matcapExternalPath = path;
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
                const CullFlagsBits values[] = {CullFlagsBits::Frustum, CullFlagsBits::Occlusion};
                ImGui::SeparatorText(PSI_FILTER " Culling");
                changed |= ImBitmaskOptionPicker(GEditor.rendererConfig.cullFlags, items, values);
            }
        }
        if (IsPathTracer(GEditor.rendererMode))
        {
            {
                const char* items[] = {"Diffuse Buffer", "Specular Buffer", "Sample Count (Heatmap)"};
                const ViewFlagsBits values[] = {ViewFlagsBits::AOVDiffuse, ViewFlagsBits::AOVSpecular,
                                                ViewFlagsBits::AOVSampleCount};
                ImGui::SeparatorText(PSI_EYE_OPEN " AOV View");
                if (ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */))
                {
                    GEditor.rendererConfig.viewFlags &=
                        ~(ViewFlagsBits::BaseColor | ViewFlagsBits::Normal | ViewFlagsBits::Position |
                          ViewFlagsBits::TextureLOD | ViewFlagsBits::SHARCGrid |
                          ViewFlagsBits::SHARCOccupancy | ViewFlagsBits::SHARCRadiance);
                }
            }
        }
        {
            ImGui::SeparatorText(PSI_BUG " Debug View");
            bool debugViewChanged = false;
            if (GEditor.rendererMode == ERendererMode::RealtimePT)
            {
                const char* items[] = {
                    "BaseColor", "Normal", "Position", "Texture LOD", "SHARC Grid", "SHARC Occupancy",
                    "SHARC Cached Radiance"};
                const ViewFlagsBits values[] = {
                    ViewFlagsBits::BaseColor, ViewFlagsBits::Normal, ViewFlagsBits::Position,
                    ViewFlagsBits::TextureLOD, ViewFlagsBits::SHARCGrid, ViewFlagsBits::SHARCOccupancy,
                    ViewFlagsBits::SHARCRadiance};
                debugViewChanged =
                    ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */);
            }
            else
            {
                const char* items[] = {"BaseColor", "Normal", "Position", "Texture LOD"};
                const ViewFlagsBits values[] = {
                    ViewFlagsBits::BaseColor, ViewFlagsBits::Normal, ViewFlagsBits::Position,
                    ViewFlagsBits::TextureLOD};
                debugViewChanged =
                    ImBitmaskOptionPicker(GEditor.rendererConfig.viewFlags, items, values, true /* solo */);
            }
            if (debugViewChanged)
            {
                GEditor.rendererConfig.viewFlags &=
                    ~(ViewFlagsBits::AOVDiffuse | ViewFlagsBits::AOVSpecular | ViewFlagsBits::AOVSampleCount);
                if (IsRaster(GEditor.rendererMode))
                    changed = true;
                else if (IsPathTracer(GEditor.rendererMode))
                    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
        }
        {
            const char* items[] = {"White Base Color"};
            const MaterialFlagsBits values[] = {MaterialFlagsBits::DbgWhiteBaseColor};
            ImGui::SeparatorText(PSI_ADJUST " Material Debug");
            if (ImBitmaskOptionPicker(GEditor.rendererConfig.materialFlags, items, values, true /* solo */))
            {
                if (IsPathTracer(GEditor.rendererMode))
                    GEditor.shaderGlobals.ptAccumulatedFrames = 0;
            }
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
    if (!RHISwapchainResultMayPresent(EditorBeginFrame(renderer, GContext->presenter)))
    {
        RecreateEditorSwapchain();
        return;
    }
    ImGui_ImplFoundation_NewFrame();
    ImGui::NewFrame();

    bool cancelRendering = false;
    uint32_t targetFrames = static_cast<uint32_t>(GEditor.renderTask.targetSamples);
    double elapsed = ImGui::GetTime() - GEditor.renderTask.startTime;
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
        uint32_t completedSamples = GEditor.shaderGlobals.ptAccumulatedFrames;
        float fraction = 0.0f;
        char overlay[128];
        const char* unitStr = IsProgressive(GEditor.rendererMode) ? "samples" : "frames";

        if (GEditor.renderTask.targetTimeSeconds > 0 && GEditor.renderTask.targetSamples > 0)
        {
            float timeFrac = static_cast<float>(elapsed / GEditor.renderTask.targetTimeSeconds);
            float sampleFrac = static_cast<float>(completedSamples) / static_cast<float>(GEditor.renderTask.targetSamples);
            fraction = std::max(timeFrac, sampleFrac);
            snprintf(overlay, sizeof(overlay), "%d / %d %s (%.1fs / %ds)", completedSamples,
                     GEditor.renderTask.targetSamples, unitStr, elapsed, GEditor.renderTask.targetTimeSeconds);
        }
        else if (GEditor.renderTask.targetTimeSeconds > 0)
        {
            fraction = static_cast<float>(elapsed / GEditor.renderTask.targetTimeSeconds);
            snprintf(overlay, sizeof(overlay), "%d %s (%.1fs / %ds)", completedSamples, unitStr, elapsed, GEditor.renderTask.targetTimeSeconds);
        }
        else if (GEditor.renderTask.targetSamples > 0)
        {
            fraction = static_cast<float>(completedSamples) / static_cast<float>(GEditor.renderTask.targetSamples);
            snprintf(overlay, sizeof(overlay), "%d / %d %s (%.1fs elapsed)", completedSamples,
                     GEditor.renderTask.targetSamples, unitStr, elapsed);
        }
        else
        {
            fraction = 0.0f;
            snprintf(overlay, sizeof(overlay), "%d %s (%.1fs elapsed) - No limit", completedSamples, unitStr, elapsed);
        }

        ImGui::ProgressBar(std::clamp(fraction, 0.0f, 1.0f), ImVec2(barW, barH), overlay);
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
    GEditor.postprocessGlobals.fbWidth = GEditor.shaderGlobals.fbWidth;
    GEditor.postprocessGlobals.fbHeight = GEditor.shaderGlobals.fbHeight;
    GEditor.postprocessGlobals.viewLutIndex =
        Postprocess::ResolvePostprocessViewLutIndex(GEditor.viewLUTSdrHandle, GEditor.viewLUTHdrHandle, GContext->enableHDR);
    renderer->ExecuteFrame();
    if (!RHISwapchainResultMayPresent(EditorEndFrame(renderer, GContext->presenter)))
        RecreateEditorSwapchain();
    GEditor.shaderGlobals.ptAccumulatedFrames += GEditor.shaderGlobals.ptSamplesPerPixel;

    bool timeLimitReached = GEditor.renderTask.targetTimeSeconds > 0 && elapsed >= GEditor.renderTask.targetTimeSeconds;
    bool sampleLimitReached = GEditor.renderTask.targetSamples > 0 && GEditor.shaderGlobals.ptAccumulatedFrames >= targetFrames;

    if (cancelRendering || timeLimitReached || sampleLimitReached)
    {
        // Restore spp preview settings
        GEditor.shaderGlobals.ptSamplesPerPixel = GEditor.renderTask.previousSpp;
        GEditor.renderResolutionScale = GEditor.renderTask.previousResolutionScale;
        GEditor.rendererConfig.isRendering = false;
        if (!cancelRendering)
            DoRenderReadback(outputs);
        GEditor.state = FERunningEnter;
    }
}

static void DrawInstanceGizmos()
{
    if (!GEditor.gizmo.showImGuizmo)
        return;
    if (!IsSelectedInstanceValid())
        return;
    if (!GEditor.selectedLight.IsNil())
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
    if (!GEditor.gizmo.showImGuizmo)
        return;
    if (!GEditor.HasScene())
        return;
    auto lights = GEditor.Scene().GetLights();
    if (lights.empty() || !GEditor.viewport.HasRect())
        return;

    int const selectedIdx = SceneLightIndexFromId(GEditor.selectedLight);
    if (selectedIdx < 0 || selectedIdx >= static_cast<int>(lights.size()))
        return;

    auto& light = lights[selectedIdx];
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

static void DrawViewportSelectionContextMenu()
{
    if (GEditor.applySelectionDoubleClickAction)
    {
        GEditor.applySelectionDoubleClickAction = false;
        if (IsSelectedInstanceValid())
            FrameInstance(SelectedInstanceIndex());
        else if (IsLightIndexValid(SceneLightIndexFromId(GEditor.selectedLight)))
            AlignViewToLight(SceneLightIndexFromId(GEditor.selectedLight));
    }

    if (GEditor.openSelectionContextMenu)
    {
        ImGui::OpenPopup("ViewportSelectionContextMenu");
        GEditor.openSelectionContextMenu = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("ViewportSelectionContextMenu"))
        return;

    if (IsSelectedInstanceValid())
    {
        int const idx = SelectedInstanceIndex();
        if (ImGui::MenuItem("Frame Selected"))
            FrameInstance(idx);
        if (ImGui::MenuItem("Move to View"))
            MoveInstanceToView(idx);
        if (ImGui::MenuItem("Align with View"))
            AlignInstanceWithView(idx);
        ImGui::Separator();
        if (ImGui::MenuItem(PSI_TRASH " Delete"))
            DeleteSelectedInstance();
    }
    else if (int const lightIdx = SceneLightIndexFromId(GEditor.selectedLight);
             IsLightIndexValid(lightIdx))
    {
        bool const canTransform = IsLightTransformable(lightIdx);
        if (ImGui::MenuItem("Align View To Selected", nullptr, false, canTransform))
            AlignViewToLight(lightIdx);
        if (ImGui::MenuItem("Move to View", nullptr, false, canTransform))
            MoveLightToView(lightIdx);
        if (ImGui::MenuItem("Align with View", nullptr, false, canTransform))
            AlignLightWithView(lightIdx);
        ImGui::Separator();
        if (ImGui::MenuItem(PSI_TRASH " Remove Light", nullptr, false, canTransform))
            DeleteLight(lightIdx);
        if (!canTransform && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Environment light is required");
    }

    ImGui::EndPopup();
}
