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

static void DrawNodeGizmos();
static void DrawSelectedLightProperties();
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

static void DrawGPUScenePageStatsTable(Vector<GPUScene::PagePoolStat>& stats)
{
    size_t physicalPages = 0, leafTables = 0, liveBytes = 0, retiredBytes = 0, capacityBytes = 0, tableBytes = 0;
    for (auto const& stat : stats)
    {
        physicalPages += stat.physicalPages;
        leafTables += stat.leafTables;
        liveBytes += stat.liveBytes;
        retiredBytes += stat.retiredBytes;
        capacityBytes += stat.capacityBytes;
        tableBytes += stat.pageTableBytes;
    }
    ImGui::TextWrapped("Payload pages: %zu | Address tables: %zu roots + %zu leaves", physicalPages, stats.size(), leafTables);
    ImGui::TextWrapped("Live: %.2f MiB | Retired: %.2f MiB | Reusable: %.2f MiB",
                       BytesToMiB(liveBytes), BytesToMiB(retiredBytes), BytesToMiB(capacityBytes - liveBytes - retiredBytes));
    ImGui::TextWrapped("Payload capacity: %.2f MiB | Page tables: %.1f KiB", BytesToMiB(capacityBytes), tableBytes / 1024.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Pages stay allocated for reuse. Hover columns for details.");
    ImGui::PopTextWrapPos();

    static constexpr const char* labels[] = {"Pool", "Pages", "Allocated", "Live", "Retired", "Free", "Tables", "Fill"};
    static constexpr const char* descriptions[] = {
        "Pool names also prefix GPU buffer debug labels. Each pool has one stable page-table root.",
        "Physical payload buffers, not logical address pages. A large allocation can span several logical pages.",
        "Actual GPU allocation bytes: payload buffers plus the root and leaf tables. Includes allocator padding.",
        "Active suballocations, including upload alignment padding. Excludes retired snapshots.",
        "Superseded upload allocations not yet reclaimed. They may already be GPU-complete; reclamation runs on updates.",
        "Reusable payload capacity: capacity minus live and retired bytes. Free ranges may be fragmented.",
        "Page-table overhead: the root plus allocated leaf tables. Already included in Allocated and Tracked Total.",
        "Occupied payload capacity (live + retired). This is utilization of allocated pages, not a fixed scene budget.",
    };
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
    // ScrollX creates a child region: a zero height consumes the remaining panel space,
    // which may already be exhausted by the preceding profiler sections. Reserve all
    // rows plus the horizontal scrollbar, and let the parent Profiler scroll vertically.
    auto const& style = ImGui::GetStyle();
    float const headerHeight = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
    float const rowHeight = ImGui::GetFrameHeight() + style.CellPadding.y * 2.0f;
    float const tableHeight = headerHeight + rowHeight * static_cast<float>(stats.size()) + style.ScrollbarSize + 2.0f;
    if (!ImGui::BeginTable("##GPUScenePageStatsTable", 8, flags, ImVec2(0.0f, tableHeight)))
        return;
    for (int column = 0; column < 8; ++column)
    {
        ImGuiTableColumnFlags columnFlags = column == 2
            ? ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending : ImGuiTableColumnFlags_None;
        ImGui::TableSetupColumn(labels[column], columnFlags);
    }
    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers, headerHeight);
    for (int column = 0; column < 8; ++column)
    {
        if (!ImGui::TableSetColumnIndex(column))
            continue;
        ImGui::TableHeader(labels[column]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", descriptions[column]);
    }
    if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs(); sortSpecs && sortSpecs->SpecsCount > 0)
    {
        auto Value = [](GPUScene::PagePoolStat const& stat, int column) -> double
        {
            switch (column)
            {
            case 1: return static_cast<double>(stat.physicalPages);
            case 2: return static_cast<double>(stat.payloadBytes + stat.pageTableBytes);
            case 3: return static_cast<double>(stat.liveBytes);
            case 4: return static_cast<double>(stat.retiredBytes);
            case 5: return static_cast<double>(stat.capacityBytes - stat.liveBytes - stat.retiredBytes);
            case 6: return static_cast<double>(stat.pageTableBytes);
            case 7:
                return stat.capacityBytes ? static_cast<double>(stat.liveBytes + stat.retiredBytes) / stat.capacityBytes : 0.0;
            default: return 0.0;
            }
        };
        ImGuiTableColumnSortSpecs const& spec = sortSpecs->Specs[0];
        std::sort(stats.begin(), stats.end(), [&](auto const& lhs, auto const& rhs)
        {
            double a = Value(lhs, spec.ColumnIndex), b = Value(rhs, spec.ColumnIndex);
            int cmp = a < b ? -1 : (a > b ? 1 : 0);
            if (cmp == 0)
                cmp = std::strcmp(lhs.name.c_str(), rhs.name.c_str());
            return spec.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0 : cmp > 0;
        });
    }

    auto DrawBytes = [](size_t bytes)
    {
        if (bytes >= (1u << 20u))
            ImGui::Text("%.2f MiB", BytesToMiB(bytes));
        else if (bytes >= 1024u)
            ImGui::Text("%.1f KiB", bytes / 1024.0f);
        else
            ImGui::Text("%zu B", bytes);
    };
    for (auto const& stat : stats)
    {
        size_t const occupiedBytes = stat.liveBytes + stat.retiredBytes;
        ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
        if (ImGui::TableNextColumn())
        {
            ImGui::TextUnformatted(stat.name.c_str());
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("GPU debug labels:\nGPUScene / %s / Page N\nGPUScene / %s / Page Table Root\n"
                                  "GPUScene / %s / Page Table Leaf N", stat.name.c_str(), stat.name.c_str(), stat.name.c_str());
        }
        if (ImGui::TableNextColumn())
        {
            ImGui::Text("%zu", stat.physicalPages);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%zu physical buffers cover %zu logical pages\nLogical page size: %zu bytes\n"
                                  "1 root + %zu leaf tables", stat.physicalPages, stat.logicalPages, stat.logicalPageBytes,
                                  stat.leafTables);
        }
        if (ImGui::TableNextColumn())
        {
            DrawBytes(stat.payloadBytes + stat.pageTableBytes);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Payload allocation: %.2f MiB\nAddressable capacity: %.2f MiB\nPage tables: %.1f KiB",
                                  BytesToMiB(stat.payloadBytes), BytesToMiB(stat.capacityBytes), stat.pageTableBytes / 1024.0f);
        }
        if (ImGui::TableNextColumn())
            DrawBytes(stat.liveBytes);
        if (ImGui::TableNextColumn())
        {
            DrawBytes(stat.retiredBytes);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%zu retired allocations awaiting reclamation (not necessarily still in flight).",
                                  stat.retiredAllocations);
        }
        if (ImGui::TableNextColumn())
            DrawBytes(stat.capacityBytes - occupiedBytes);
        if (ImGui::TableNextColumn())
            ImGui::Text("%.1f KiB", stat.pageTableBytes / 1024.0f);
        if (ImGui::TableNextColumn())
        {
            if (stat.capacityBytes)
            {
                float fill = static_cast<float>(occupiedBytes) / static_cast<float>(stat.capacityBytes);
                char overlay[32];
                std::snprintf(overlay, sizeof(overlay), "%.1f%%", fill * 100.0f);
                ImGui::ProgressBar(fill, ImVec2(ImGui::GetFontSize() * 5.0f, 0.0f), overlay);
            }
            else
                ImGui::TextDisabled("No payload");
        }
    }
    ImGui::EndTable();
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

static int SelectedInstanceIndex()
{
    return SceneInstanceIndexFromId(GEditor.selectedInstance);
}

static bool IsNodeTransformable(FUUID nodeId)
{
    int const index = SceneNodeIndexFromId(nodeId);
    if (index < 0)
        return false;
    auto const& scene = GEditor.Scene();
    int const light = scene.LightIndex(scene.GetNodes()[index].light);
    return light < 0 || scene.GetLights()[light].type != FLightType::Environment;
}

static void FrameNode(FUUID nodeId)
{
    FSerializedBounds bounds{};
    if (!SceneNodeWorldBounds(nodeId, bounds))
        return;

    float3 center = (bounds.min + bounds.max) * 0.5f;
    float radius = length(bounds.max - center);
    float halfFovY = GEditor.camera.fovY * 0.5f;
    float halfFovX = std::atan(std::tan(halfFovY) * std::max(GEditor.camera.aspect, 1e-3f));
    float halfFov = std::max(std::min(halfFovX, halfFovY), 1e-3f);

    GEditor.camera.center = center;
    GEditor.camera.radius = std::max(radius / std::sin(halfFov), 1e-3f);
    GEditor.cameraUpdated = true;
    ++GEditor.shaderGlobals.ptDenoiseHistoryEpoch;
}

static void MoveNodeToView(FUUID nodeId, bool align = false)
{
    if (!IsNodeTransformable(nodeId))
        return;
    auto& scene = GEditor.Scene();
    mat4 world = scene.NodeWorldTransform(nodeId);
    if (align)
    {
        FTransform transform;
        Math::decompose(world, transform.scale, transform.rotation, transform.transform);
        transform.transform = GEditor.camera.position;
        transform.rotation = GEditor.camera.rot;
        world = SceneTransformMatrix(transform);
    }
    else
        world[3] = float4(GEditor.camera.position, 1.0f);
    if (scene.SetNodeWorldTransform(nodeId, world))
        CommitSceneToGPU(true);
    else
        LOG(Editor, LogWarn, "Cannot move node through a singular parent transform");
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
    ++GEditor.shaderGlobals.ptDenoiseHistoryEpoch;
}

// Returns a requested deletion; callers apply it after finishing hierarchy traversal.
static bool DrawNodeActions(FUUID nodeId)
{
    bool const canTransform = IsNodeTransformable(nodeId);
    FSerializedBounds bounds{};
    if (ImGui::MenuItem("Frame Selected", nullptr, false, SceneNodeWorldBounds(nodeId, bounds)))
        FrameNode(nodeId);
    int const light = SceneLightIndexFromId(GEditor.selectedLight);
    if (IsLightTransformable(light) && ImGui::MenuItem("Align View To Selected"))
        AlignViewToLight(light);
    if (ImGui::MenuItem("Move to View", nullptr, false, canTransform))
        MoveNodeToView(nodeId);
    if (ImGui::MenuItem("Align with View", nullptr, false, canTransform))
        MoveNodeToView(nodeId, true);
    ImGui::Separator();
    bool const canRemove = GEditor.HasScene() && GEditor.Scene().CanRemoveNode(nodeId);
    bool const remove = ImGui::MenuItem(PSI_TRASH " Delete Subtree", nullptr, false, canRemove);
    if (!canRemove && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The environment light is required");
    return remove;
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

static void CopyRenderGraphGraphviz(Renderer const* renderer)
{
    CHECK(renderer);
    String graphviz = renderer->DbgDumpGraphviz();
    ImGui::SetClipboardText(graphviz.c_str());
}

struct RenderGraphModalState
{
    bool pendingOpen{};
    bool fitPending{};
    float zoom{1.0f};
    ImVec2 pan{};
};

static constexpr char const* kRenderGraphModalName = "Render Graph Visualization";

static RenderGraphModalState& RenderGraphModal()
{
    static RenderGraphModalState state;
    return state;
}

static void OpenRenderGraphModal()
{
    auto& state = RenderGraphModal();
    state.pendingOpen = true;
    state.fitPending = true;
    state.zoom = 1.0f;
    state.pan = {};
}

static void DrawRenderGraphModal()
{
    auto& state = RenderGraphModal();
    if (state.pendingOpen)
    {
        ImGui::OpenPopup(kRenderGraphModalName);
        state.pendingOpen = false;
    }

    ImGui::SetNextWindowSize(ImVec2(1000.0f, 700.0f), ImGuiCond_FirstUseEver);
    bool modalOpen = true;
    if (!ImGui::BeginPopupModal(kRenderGraphModalName, &modalOpen,
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
        return;

    if (!modalOpen)
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    Renderer* renderer = GContext ? GContext->renderer : nullptr;
    if (!renderer)
    {
        ImGui::TextUnformatted("Render Graph is unavailable.");
        ImGui::EndPopup();
        return;
    }

    Allocator* scratch = GContext->editorFrameScratch ? GContext->editorFrameScratch.get() : GLOBAL_ALLOC;
    Vector<Renderer::DebugGraphNode> nodes(scratch);
    Vector<Renderer::DebugGraphEdge> edges(scratch);
    renderer->DbgGetGraph(nodes, edges);
    Ranges::sort(nodes, [](auto const& a, auto const& b) { return a.ord < b.ord; });

    size_t activeCount = 0;
    size_t maxDepth = 0;
    PassHandle maxHandle = 0;
    for (auto const& node : nodes)
    {
        maxHandle = std::max(maxHandle, node.handle);
        if (!node.used)
            continue;
        activeCount++;
        maxDepth = std::max(maxDepth, node.depth);
    }

    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Zoom", &state.zoom, 0.1f, 3.0f, "%.2fx");
    ImGui::SameLine();
    if (ImGui::Button(PSI_REFRESH " Fit"))
        state.fitPending = true;
    ImGui::SameLine();
    if (ImGui::Button(PSI_REMOVE " Close"))
    {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu passes, %zu dependencies | Wheel: zoom, drag: pan", activeCount, edges.size());

    if (activeCount == 0)
    {
        ImGui::TextUnformatted("No active passes.");
        ImGui::EndPopup();
        return;
    }

    constexpr float nodeWidth = 180.0f;
    constexpr float nodeHeight = 44.0f;
    constexpr float horizontalSpacing = 260.0f;
    constexpr float verticalSpacing = 72.0f;

    Vector<size_t> layerCounts(maxDepth + 1, 0, scratch);
    for (auto const& node : nodes)
        if (node.used)
            layerCounts[node.depth]++;
    size_t maxRows = *Ranges::max_element(layerCounts);

    Vector<size_t> layerRows(maxDepth + 1, 0, scratch);
    Vector<ImVec2> positions(static_cast<size_t>(maxHandle) + 1, ImVec2{}, scratch);
    Vector<uint8_t> visible(static_cast<size_t>(maxHandle) + 1, 0, scratch);
    for (auto const& node : nodes)
    {
        if (!node.used)
            continue;
        float x = static_cast<float>(maxDepth - node.depth) * horizontalSpacing;
        float rowOffset = static_cast<float>(maxRows - layerCounts[node.depth]) * 0.5f;
        float y = (rowOffset + static_cast<float>(layerRows[node.depth]++)) * verticalSpacing;
        positions[node.handle] = {x, y};
        visible[node.handle] = 1;
    }

    float graphWidth = static_cast<float>(maxDepth) * horizontalSpacing + nodeWidth;
    float graphHeight = static_cast<float>(maxRows - 1) * verticalSpacing + nodeHeight;
    ImVec2 graphCenter{graphWidth * 0.5f, graphHeight * 0.5f};

    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(canvasSize.x, 1.0f);
    canvasSize.y = std::max(canvasSize.y, 1.0f);
    ImGui::BeginChild("RenderGraphCanvas", canvasSize, true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 innerSize = ImGui::GetContentRegionAvail();
    innerSize.x = std::max(innerSize.x, 1.0f);
    innerSize.y = std::max(innerSize.y, 1.0f);
    ImGui::InvisibleButton("RenderGraphDragSurface", innerSize,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);

    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 canvasCenter{canvasPos.x + innerSize.x * 0.5f, canvasPos.y + innerSize.y * 0.5f};
    if (state.fitPending)
    {
        float fitX = std::max(innerSize.x - 60.0f, 1.0f) / graphWidth;
        float fitY = std::max(innerSize.y - 60.0f, 1.0f) / graphHeight;
        state.zoom = std::clamp(std::min(fitX, fitY), 0.1f, 1.5f);
        state.pan = {};
        state.fitPending = false;
    }
    if (hovered && io.MouseWheel != 0.0f)
    {
        float oldZoom = state.zoom;
        float newZoom = std::clamp(oldZoom * (io.MouseWheel > 0.0f ? 1.1f : 1.0f / 1.1f), 0.1f, 3.0f);
        float zoomRatio = newZoom / oldZoom;
        ImVec2 mouseFromCenter{io.MousePos.x - canvasCenter.x - state.pan.x,
                               io.MousePos.y - canvasCenter.y - state.pan.y};
        state.pan.x += mouseFromCenter.x * (1.0f - zoomRatio);
        state.pan.y += mouseFromCenter.y * (1.0f - zoomRatio);
        state.zoom = newZoom;
    }
    if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) ||
                    ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
    {
        state.pan.x += io.MouseDelta.x;
        state.pan.y += io.MouseDelta.y;
    }

    auto transform = [&](ImVec2 point)
    {
        return ImVec2{
            canvasCenter.x + state.pan.x + (point.x - graphCenter.x) * state.zoom,
            canvasCenter.y + state.pan.y + (point.y - graphCenter.y) * state.zoom,
        };
    };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasMax{canvasPos.x + innerSize.x, canvasPos.y + innerSize.y};
    drawList->AddRectFilled(canvasPos, canvasMax, IM_COL32(18, 18, 18, 255));
    drawList->PushClipRect(canvasPos, canvasMax, true);

    float gridSpacing = 32.0f * state.zoom;
    if (gridSpacing >= 8.0f)
    {
        float offsetX = std::fmod(canvasCenter.x + state.pan.x, gridSpacing);
        float offsetY = std::fmod(canvasCenter.y + state.pan.y, gridSpacing);
        for (float x = canvasPos.x + offsetX; x < canvasMax.x; x += gridSpacing)
            drawList->AddLine({x, canvasPos.y}, {x, canvasMax.y}, IM_COL32(255, 255, 255, 12));
        for (float y = canvasPos.y + offsetY; y < canvasMax.y; y += gridSpacing)
            drawList->AddLine({canvasPos.x, y}, {canvasMax.x, y}, IM_COL32(255, 255, 255, 12));
    }

    for (auto const& edge : edges)
    {
        if (edge.consumer >= visible.size() || edge.producer >= visible.size() ||
            !visible[edge.consumer] || !visible[edge.producer])
            continue;
        ImVec2 consumer = positions[edge.consumer];
        ImVec2 producer = positions[edge.producer];
        ImVec2 start = transform({producer.x + nodeWidth, producer.y + nodeHeight * 0.5f});
        ImVec2 end = transform({consumer.x, consumer.y + nodeHeight * 0.5f});
        float middleX = (start.x + end.x) * 0.5f;
        ImU32 edgeColor = IM_COL32(180, 190, 205, 150);
        drawList->AddLine(start, {middleX, start.y}, edgeColor, 1.5f);
        drawList->AddLine({middleX, start.y}, {middleX, end.y}, edgeColor, 1.5f);
        drawList->AddLine({middleX, end.y}, end, edgeColor, 1.5f);
        float arrowSize = std::clamp(6.0f * state.zoom, 3.0f, 7.0f);
        drawList->AddTriangleFilled(end, {end.x - arrowSize, end.y - arrowSize * 0.65f},
                                   {end.x - arrowSize, end.y + arrowSize * 0.65f}, edgeColor);
    }

    for (auto const& node : nodes)
    {
        if (!node.used)
            continue;
        ImVec2 nodeMin = transform(positions[node.handle]);
        ImVec2 nodeMax{nodeMin.x + nodeWidth * state.zoom, nodeMin.y + nodeHeight * state.zoom};
        bool nodeHovered = hovered && io.MousePos.x >= nodeMin.x && io.MousePos.x <= nodeMax.x &&
                           io.MousePos.y >= nodeMin.y && io.MousePos.y <= nodeMax.y;
        ImU32 fill = node.epilogue ? IM_COL32(45, 150, 70, 255)
                                  : node.queue == RHIDeviceQueueType::Graphics ? IM_COL32(180, 105, 45, 255)
                                                                              : IM_COL32(45, 135, 95, 255);
        drawList->AddRectFilled(nodeMin, nodeMax, fill, 5.0f);
        drawList->AddRect(nodeMin, nodeMax,
                          nodeHovered ? IM_COL32(255, 220, 100, 255) : IM_COL32(0, 0, 0, 180),
                          5.0f, 0, nodeHovered ? 2.0f : 1.0f);

        if (state.zoom >= 0.55f)
        {
            drawList->PushClipRect({nodeMin.x + 6.0f, nodeMin.y + 3.0f},
                                   {nodeMax.x - 4.0f, nodeMax.y - 3.0f}, true);
            drawList->AddText({nodeMin.x + 7.0f, nodeMin.y + 4.0f}, IM_COL32(255, 255, 255, 255),
                              node.name.data(), node.name.data() + node.name.size());
            char details[96];
            std::snprintf(details, sizeof(details), "#%zu  depth %zu  group %d",
                          static_cast<size_t>(node.handle), node.depth, node.groupIndex);
            drawList->AddText({nodeMin.x + 7.0f, nodeMin.y + 23.0f}, IM_COL32(235, 235, 235, 190), details);
            drawList->PopClipRect();
        }

        if (nodeHovered)
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(node.name.data(), node.name.data() + node.name.size());
            ImGui::Text("Pass #%zu | Depth %zu | Order %zu | Group %d", static_cast<size_t>(node.handle),
                        node.depth, node.ord, node.groupIndex);
            ImGui::TextUnformatted(node.queue == RHIDeviceQueueType::Graphics ? "Graphics queue" : "Compute queue");
            ImGui::EndTooltip();
        }
    }

    drawList->PopClipRect();
    ImGui::EndChild();
    ImGui::EndPopup();
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
        ImGui::DockBuilderDockWindow("Rendering", dockRight);
        ImGui::DockBuilderDockWindow("Profiler", dockRight);
        ImGui::DockBuilderFinish(dockspaceID);
    }

    // Main menu bar
    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu(PSI_FOLDER_CLOSE " File"))
        {
            if (ImGui::MenuItem(PSI_FOLDER_OPEN " Open Scene (Replace)"))
            {
                String path;
                if (OpenFile({"Scene Files", "gltf;glb;fscn"}, path))
                    RequestLoadScene(path);
            }
            if (ImGui::MenuItem(PSI_PLUS_SIGN " Import Scene (Add)"))
            {
                String path;
                if (OpenFile({"Scene Files", "gltf;glb;fscn"}, path))
                    RequestAppendScene(path);
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
            bool const progressiveRender = IsProgressive(GEditor.rendererMode);
            bool const rasterTAARender = IsRaster(GEditor.rendererMode) && GEditor.rasterTAA;
            uint32_t const rasterTAAFrames = std::clamp(GEditor.rasterTAAConfig.sampleCount, 2u, 32u);
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
            else if (rasterTAARender)
            {
                ImGui::Text("TAA accumulation: %u frames", rasterTAAFrames);
            }
            else
            {
                ImGui::TextUnformatted("This mode captures the next rendered frame.");
            }
            if (ImGui::Button(PSI_PLAY " Start Render"))
            {
                GEditor.renderTask.targetSamples = progressiveRender
                    ? GEditor.renderTask.samplePopupInput
                    : (rasterTAARender ? static_cast<int>(rasterTAAFrames) : 1);
                GEditor.renderTask.targetTimeSeconds = progressiveRender ? GEditor.renderTask.timePopupInput : 0;
                GEditor.renderTask.startTime = ImGui::GetTime();
                GEditor.renderTask.renderPaused = false;
                GEditor.renderTask.renderAutoPaused = false;
                GEditor.shaderGlobals.accumulatedFrames = 0;
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

static void AddHierarchyNode(Optional<FLightType> lightType = {})
{
    auto& scene = GEditor.Scene();
    FUUID const parent = IsNodeTransformable(GEditor.selectedNode) ? GEditor.selectedNode : kNilUUID;
    String const name = lightType ? Format("{} Light", LightTypeName(*lightType)) : String("Node");
    FSceneNode node{.id = FUUID::Generate(), .name = scene.InternString(name.c_str()),
                    .parent = lightType ? kNilUUID : parent};
    if (lightType)
    {
        FLight light{};
        light.id = FUUID::Generate();
        light.name = node.name;
        light.type = *lightType;
        light.color = float3{1.0f, 0.92f, 0.78f};
        light.power = *lightType == FLightType::Directional ? 1.0f : 10.0f;
        light.radius = 0.05f;
        light.angularDiameter = radians(2.0f);
        node.light = light.id;
        node.localTransform = translate(mat4(1.0f), GEditor.camera.position) * mat4_cast(GEditor.camera.rot);
        scene.mTables.lights.push_back(light);
    }
    scene.mTables.nodes.push_back(node);
    scene.RebuildIndex();
    // New lights start at the current view, even when inserted under a transformed parent.
    if (lightType && !parent.IsNil() && !scene.ReparentNode(node.id, parent))
        LOG(Editor, LogWarn, "Added light at scene root because the selected parent transform is singular");
    SelectNode(node.id);
    CommitSceneToGPU(true);
}

static void DrawHierarchyAddMenu()
{
    if (ImGui::Button(PSI_PLUS_SIGN " Add"))
        ImGui::OpenPopup("HierarchyAdd");
    if (!ImGui::BeginPopup("HierarchyAdd"))
        return;
    if (ImGui::MenuItem("Empty Node"))
        AddHierarchyNode();
    if (ImGui::BeginMenu("Light"))
    {
        for (uint32_t type = static_cast<uint32_t>(FLightType::Directional);
             type <= static_cast<uint32_t>(FLightType::Rect); ++type)
            if (ImGui::MenuItem(LightTypeName(static_cast<FLightType>(type))))
                AddHierarchyNode(static_cast<FLightType>(type));
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Punctual Light Units"))
    {
        float factor = 1.0f;
        if (ImGui::MenuItem("To SI"))
            factor = 683.0f;
        if (ImGui::MenuItem("To Unitless"))
            factor = 1.0f / 683.0f;
        if (factor != 1.0f)
        {
            for (FLight& light : GEditor.Scene().GetLights())
                if (light.type == FLightType::Point || light.type == FLightType::Spot ||
                    light.type == FLightType::Directional)
                    light.power *= factor;
            CommitSceneToGPU(true);
        }
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

static void DrawHierarchyTree()
{
    auto& scene = GEditor.Scene();
    DrawHierarchyAddMenu();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu nodes / %zu instances / %zu lights", scene.GetNodes().size(),
                        scene.GetInstances().size(), scene.GetLights().size());
    static char nameFilter[128]{};
    static int typeFilter = 0;
    static constexpr char const* typeNames[] = {"All types", "Mesh", "Light", "Env", "Node (Root)"};
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::InputTextWithHint("##HierarchyNameFilter", "Search by name...", nameFilter, sizeof(nameFilter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##HierarchyTypeFilter", &typeFilter, typeNames, IM_ARRAYSIZE(typeNames));
    ImGui::Separator();
    auto const nodes = scene.GetNodes();
    auto const& hierarchy = scene.mHierarchy;
    constexpr uint32_t invalid = FSceneHierarchy::Invalid;
    bool const filtering = nameFilter[0] != '\0' || typeFilter != 0;
    Vector<uint8_t> visible(filtering ? nodes.size() : 0u, 0u, GLOBAL_ALLOC);
    if (filtering)
    {
        for (uint32_t index : hierarchy.order)
        {
            FSceneNode const& node = nodes[index];
            int const light = scene.LightIndex(node.light);
            int const instance = scene.InstanceIndex(node.instance);
            bool typeMatches = false;
            switch (typeFilter)
            {
            case 0: typeMatches = true; break;
            case 1: typeMatches = instance >= 0 && scene.GetInstances()[instance].type == FInstanceType::Mesh; break;
            case 2: typeMatches = light >= 0 && scene.GetLights()[light].type != FLightType::Environment; break;
            case 3: typeMatches = light >= 0 && scene.GetLights()[light].type == FLightType::Environment; break;
            case 4:
                typeMatches = hierarchy.parents[index] == invalid && instance < 0 && light < 0 && node.camera.IsNil();
                break;
            }
            bool nameMatches = true;
            if (nameFilter[0] != '\0')
            {
                FUUID const nameId = node.name.IsNil() && light >= 0 ? scene.GetLights()[light].name : node.name;
                char const* name = ResolveSceneName(nameId);
                nameMatches = name && ImStristr(name, nullptr, nameFilter, nullptr) != nullptr;
            }
            visible[index] = typeMatches && nameMatches;
        }
        // Include ancestors of matches so filtered children remain reachable.
        for (auto it = hierarchy.order.rbegin(); it != hierarchy.order.rend(); ++it)
        {
            uint32_t const parent = hierarchy.parents[*it];
            if (visible[*it] && parent != invalid)
                visible[parent] = 1;
        }
    }
    Vector<uint8_t> revealPath(GEditor.revealSelectedNode ? nodes.size() : 0u, 0u, GLOBAL_ALLOC);
    if (!revealPath.empty())
    {
        int index = scene.NodeIndex(GEditor.selectedNode);
        while (index >= 0)
        {
            revealPath[index] = 1;
            uint32_t const parent = hierarchy.parents[index];
            index = parent == invalid ? -1 : static_cast<int>(parent);
        }
    }
    Vector<uint32_t> pending(GLOBAL_ALLOC);
    pending.assign(hierarchy.roots.rbegin(), hierarchy.roots.rend());
    FUUID deleteNode{};
    bool anyVisible = false;
    // Explicit stack also handles very deep imported hierarchies without recursive UI calls.
    while (!pending.empty())
    {
        uint32_t const index = pending.back();
        pending.pop_back();
        if (index == invalid)
        {
            ImGui::TreePop();
            continue;
        }
        if (filtering && !visible[index])
            continue;
        anyVisible = true;
        FSceneNode const& node = nodes[index];
        bool leaf = hierarchy.firstChild[index] == invalid;
        if (filtering && !leaf)
        {
            leaf = true;
            for (uint32_t child = hierarchy.firstChild[index]; child != invalid; child = hierarchy.nextSibling[child])
                if (visible[child])
                {
                    leaf = false;
                    break;
                }
        }
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (leaf)
            flags |= ImGuiTreeNodeFlags_Leaf;
        if (hierarchy.parents[index] == invalid)
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        if (GEditor.highlightSelectedNodeInHierarchy && GEditor.selectedNode == node.id)
            flags |= ImGuiTreeNodeFlags_Selected;
        if ((filtering && !leaf) || (!revealPath.empty() && revealPath[index]))
            ImGui::SetNextItemOpen(true);
        FUUID name = node.name;
        char const* kind = "Node";
        int const light = scene.LightIndex(node.light);
        int const instance = scene.InstanceIndex(node.instance);
        if (light >= 0)
        {
            kind = LightTypeName(scene.GetLights()[light].type);
            if (name.IsNil())
                name = scene.GetLights()[light].name;
        }
        else if (instance >= 0)
            kind = scene.GetInstances()[instance].type == FInstanceType::Curve ? "Curve" : "Mesh";
        else if (!node.camera.IsNil())
            kind = "Camera";
        char label[256];
        FormatIndexLabelWithName(label, sizeof(label), kind, static_cast<int>(index), name);
        ImGuiPushUUID(node.id);
        bool const open = ImGui::TreeNodeEx("##Node", flags, "%s", label);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
            SelectNode(node.id);
        if (ImGui::IsItemHovered())
            SetUUIDTooltip(node.id);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            SelectNode(node.id);
            if (IsLightTransformable(light))
                AlignViewToLight(light);
            else
                FrameNode(node.id);
        }
        if (GEditor.selectedNode == node.id && GEditor.revealSelectedNode)
        {
            ImGui::SetScrollHereY(0.5f);
            GEditor.revealSelectedNode = false;
        }
        if (ImGui::BeginPopupContextItem())
        {
            if (GEditor.selectedNode != node.id)
                SelectNode(node.id);
            if (DrawNodeActions(node.id))
                deleteNode = node.id;
            ImGui::EndPopup();
        }
        ImGui::PopID();
        if (open && !leaf)
        {
            char id[40];
            ImGui::TreePush(node.id.Format(id, sizeof(id)));
            pending.push_back(invalid);
            size_t const first = pending.size();
            for (uint32_t child = hierarchy.firstChild[index]; child != invalid; child = hierarchy.nextSibling[child])
                pending.push_back(child);
            std::reverse(pending.begin() + static_cast<std::ptrdiff_t>(first), pending.end());
        }
    }
    if (filtering && !anyVisible)
        ImGui::TextDisabled("No matching nodes");
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        ImGui::IsKeyPressed(ImGuiKey_Delete, false))
    {
        int const selectedIndex = scene.NodeIndex(GEditor.selectedNode);
        if (!filtering || (selectedIndex >= 0 && visible[selectedIndex]))
            deleteNode = GEditor.selectedNode;
    }
    if (!deleteNode.IsNil())
        DeleteSceneNode(deleteNode);
}

void FHierarchyPanel()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Hierarchy"))
    {
        if (!GEditor.HasScene())
            ImGui::TextDisabled("No scene loaded");
        else
            DrawHierarchyTree();
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

    // Inspector panel: one transform owner for meshes, groups, cameras, and lights.
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.06f, 0.70f));
    if (ImGui::Begin("Inspector"))
    {
        int const index = SceneNodeIndexFromId(GEditor.selectedNode);
        if (index >= 0)
        {
            auto& scene = GEditor.Scene();
            FSceneNode& node = scene.GetNodes()[index];
            ImGuiPushUUID(node.id);
            DrawUUIDRow(node.id);
            static FUUID namedNode{};
            static char nodeName[256]{};
            if (namedNode != node.id)
            {
                namedNode = node.id;
                std::snprintf(nodeName, sizeof(nodeName), "%s", NameOr(node.name, ""));
            }
            bool const rename = ImGui::InputText("Name", nodeName, sizeof(nodeName), ImGuiInputTextFlags_EnterReturnsTrue);
            if (rename || ImGui::IsItemDeactivatedAfterEdit())
                node.name = scene.InternString(nodeName);

            bool const canTransform = IsNodeTransformable(node.id);
            if (canTransform)
            {
                int const parentIndex = scene.NodeIndex(node.parent);
                char const* parentName = parentIndex < 0 ? "(Scene root)" :
                                            NameOr(scene.GetNodes()[parentIndex].name, "Node");
                FUUID newParent = node.parent;
                bool reparent = false;
                if (ImGui::BeginCombo("Parent", parentName))
                {
                    if (ImGui::Selectable("(Scene root)", node.parent.IsNil()))
                    {
                        newParent = kNilUUID;
                        reparent = true;
                    }
                    for (FSceneNode const& candidate : scene.GetNodes())
                    {
                        if (!IsNodeTransformable(candidate.id) || scene.IsNodeInSubtree(candidate.id, node.id))
                            continue;
                        ImGuiPushUUID(candidate.id);
                        if (ImGui::Selectable(NameOr(candidate.name, "Node"), candidate.id == node.parent))
                        {
                            newParent = candidate.id;
                            reparent = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
                if (reparent && newParent != node.parent)
                {
                    if (scene.ReparentNode(node.id, newParent))
                    {
                        GEditor.revealSelectedNode = true;
                        CommitSceneToGPU(true);
                    }
                    else
                        LOG(Editor, LogWarn, "Cannot reparent node: invalid hierarchy or singular parent transform");
                }

                ImGui::SeparatorText("Local Transform");
                FTransform local;
                Math::decompose(node.localTransform, local.scale, local.rotation, local.transform);
                bool const translationChanged = ImGui::DragFloat3("Position", &local.transform.x, 0.01f);
                bool basisChanged = ImGui::DragFloat4("Rotation", &local.rotation.x, 0.001f);
                basisChanged |= ImGui::DragFloat3("Scale", &local.scale.x, 0.01f);
                if (basisChanged)
                {
                    local.rotation = dot(local.rotation, local.rotation) > 0.0f ? normalize(local.rotation) :
                                                                                 quat_cast(mat3(1.0f));
                    node.localTransform = SceneTransformMatrix(local);
                }
                else if (translationChanged)
                    node.localTransform[3] = float4(local.transform, 1.0f);
                if (translationChanged || basisChanged)
                    CommitSceneToGPU(true);
                ImGui::TextDisabled("Relative to parent; descendants inherit this transform.");

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
            }
            if (IsSelectedInstanceValid())
            {
                GSInstance const instance = GContext->gpuScene->GetInstance(static_cast<uint32_t>(SelectedInstanceIndex()));
                ImGui::SeparatorText("Geometry");
                DrawUUIDRow(GEditor.selectedInstance);
                ImGui::Text("Type: %s", InstanceTypeName(instance.type));
                ImGui::Text("Resource Offset: %u", instance.resourceOffset);
                ImGui::Text("Material Index: %u", instance.materialIndex);
                ImGui::Text("Resource Index: %u", instance.resourceIndex);
            }
            DrawSelectedLightProperties();
            ImGui::PopID();
        }
        else
            ImGui::TextDisabled("No node selected");
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

static void DrawSelectedLightProperties()
{
    if (!GEditor.HasScene())
        return;
    auto lights = GEditor.Scene().GetLights();
    static const char* kLightTypeNames[] = {"Directional", "Point", "Spot", "Disk", "Rect"};

    int const index = SceneLightIndexFromId(GEditor.selectedLight);
    if (index < 0 || index >= static_cast<int>(lights.size()))
        return;
    FLight& light = lights[index];
    bool const isEnvironment = light.type == FLightType::Environment;
    bool changed = false;
    ImGui::SeparatorText(PSI_BOLT " Light");
    ImGuiPushUUID(light.id);
    DrawUUIDRow(light.id);
    if (isEnvironment)
        ImGui::TextDisabled("Environment light is scene-global and is not removable.");
    else
    {
        int type = static_cast<int>(light.type) - 1;
        if (ImGui::Combo("Type", &type, kLightTypeNames, IM_ARRAYSIZE(kLightTypeNames)))
        {
            light.type = static_cast<FLightType>(type + 1);
            changed = true;
        }
    }
    changed |= ImHDRColorEdit(isEnvironment ? "Ambient" : "Color", light.color, light.power);
    if (isEnvironment)
    {
        bool const hasEnv = GContext->gpuScene && GContext->gpuScene->HasEnvironmentMap();
        ImGui::TextUnformatted(hasEnv ? PSI_OK " HDRI Loaded" : PSI_WARNING_SIGN " No HDRI");
        if (hasEnv)
        {
            DrawTexturePreview("HDRI", GContext->gpuScene->mEnvMapIndex.index);
            if (!light.environmentTexture.IsNil())
            {
                uint32_t const texture = SceneTextureIndexFromId(light.environmentTexture);
                if (texture != kInvalidTexture)
                {
                    char const* name = NameOr(GEditor.Scene().GetTextures()[texture].name, nullptr);
                    if (name)
                        ImGui::TextDisabled("Source: %s", name);
                }
                DrawUUIDRow(light.environmentTexture);
            }
            changed |= ImGui::SliderFloat("Azimuth Offset", &light.environmentAzimuthOffset, -180.0f, 180.0f, "%.1f deg");
            changed |= ImGui::Checkbox("Enable Env Map", &light.environmentMap);
        }
        ImGui::TextDisabled(PSI_UPLOAD " Drag & drop .hdr/.hdri to load");
    }
    if (light.type == FLightType::Directional)
    {
        float diameter = degrees(light.angularDiameter);
        if (ImGui::SliderFloat("Angular Diameter", &diameter, 0.0f, 180.0f, "%.2f deg"))
        {
            light.angularDiameter = radians(diameter);
            changed = true;
        }
    }
    if (light.type == FLightType::Point || light.type == FLightType::Spot)
        changed |= ImGui::DragFloat("Radius", &light.radius, 0.01f, 0.0f, 100.0f, "%.3f");
    if (light.type == FLightType::Spot)
    {
        float inner = degrees(light.spotInnerConeAngle);
        float outer = degrees(light.spotOuterConeAngle);
        bool coneChanged = ImGui::SliderFloat("Inner Cone", &inner, 0.0f, outer, "%.1f deg");
        coneChanged |= ImGui::SliderFloat("Outer Cone", &outer, inner, 90.0f, "%.1f deg");
        if (coneChanged)
        {
            light.spotInnerConeAngle = radians(inner);
            light.spotOuterConeAngle = radians(outer);
            changed = true;
        }
    }
    if (light.type == FLightType::Disk || light.type == FLightType::Rect)
    {
        changed |= ImGui::DragFloat("Half Width", &light.width, 0.01f, 0.001f, 100.0f, "%.3f");
        changed |= ImGui::DragFloat("Half Height", &light.height, 0.01f, 0.001f, 100.0f, "%.3f");
        changed |= ImGui::Checkbox("Two-Sided", &light.twoSided);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Normalize", &light.normalize);
    }
    if (!isEnvironment)
        changed |= ImGui::Checkbox("Shadows", &light.useShadow);
    if (changed)
        CommitSceneToGPU(true);
    ImGui::PopID();
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
        const char* projectionItems[] = {
            "Perspective",
            "Panoramic (Equirectangular)",
            "Panini",
            "Fisheye (Equidistant)",
        };
        int cameraProjection = static_cast<int>(GEditor.shaderGlobals.cameraProjection);
        if (ImGui::Combo("Projection", &cameraProjection, projectionItems, IM_ARRAYSIZE(projectionItems)))
        {
            GEditor.shaderGlobals.cameraProjection = to_integer(static_cast<CameraProjection>(cameraProjection));
            GEditor.cameraUpdated = true;
        }
        CameraProjection projection = static_cast<CameraProjection>(GEditor.shaderGlobals.cameraProjection);
        bool panoramicCamera = projection == CameraProjection::Panoramic;
        if (!panoramicCamera)
            GEditor.cameraUpdated |= ImGui::SliderAngle("Cam FOV Y", &GEditor.camera.fovY, 1.0f, 179.0f);
        if (panoramicCamera)
            ImGui::TextDisabled("Renders a 360x180 equirectangular view.");
        else if (projection == CameraProjection::Panini)
            ImGui::TextDisabled("Compresses wide views while keeping vertical lines straight.");
        else if (projection == CameraProjection::Fisheye)
            ImGui::TextDisabled("Uses an equidistant full-frame fisheye mapping.");
        GEditor.cameraUpdated |= ImGui::SliderFloat("Z Near", &GEditor.camera.zNear, 0.001f, 10.0f, "%.4f",
                                                    ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Exposure (EV)", &GEditor.shaderGlobals.camEV, -16.0f, 16.0f);
        ImGui::Separator();
        ImGui::SliderFloat("WASD Speed", &GEditor.camera.moveSpeed, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        if (!panoramicCamera)
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
            ImGui::TextDisabled("No GPU timing data");
        // Memory/page diagnostics remain useful before GPU timestamps are available.
        if (GContext->device)
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
                    Vector<GPUScene::PagePoolStat> pages(scratch);
                    GContext->gpuScene->DbgGetMemoryStatistics(stats, &pages);
                    size_t totalBytes = 0;
                    for (auto const& stat : stats)
                        totalBytes += stat.bytes;
                    ImGui::Text("Tracked Total: %.2f MiB", BytesToMiB(totalBytes));
                    if (ImGui::TreeNodeEx("Buffer Pages", ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        DrawGPUScenePageStatsTable(pages);
                        ImGui::TreePop();
                    }
                    if (ImGui::TreeNode("Allocation Breakdown"))
                    {
                        DrawMemoryStatsTable("##GPUSceneMemoryTable", stats);
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
            if (ImGui::TreeNodeEx(PSI_PICTURE " Render Graph", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (GContext->renderer)
                {
                    if (ImGui::Button(PSI_COPY " Copy GraphViz"))
                        CopyRenderGraphGraphviz(GContext->renderer);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Copy the current Render Graph GraphViz source");
                    ImGui::SameLine();
                    if (ImGui::Button(PSI_SITEMAP " Open Graph"))
                        OpenRenderGraphModal();
                    Allocator* scratch = GContext->editorFrameScratch ? GContext->editorFrameScratch.get() : GLOBAL_ALLOC;
                    Vector<Renderer::MemoryStat> stats(scratch);
                    GContext->renderer->DbgGetMemoryStatistics(stats);
                    size_t totalBytes = DrawMemoryStatsTable("##RenderGraphMemoryTable", stats);
                    ImGui::Text("Renderer-Owned Total: %.1f MB", totalBytes / static_cast<float>(1 << 20u));
                    DrawRenderGraphTexturePreviews(GContext->renderer, scratch);
                }
                ImGui::TreePop();
            }
            if (!timings.empty() && ImGui::TreeNodeEx(PSI_TIME " Frametime", ImGuiTreeNodeFlags_DefaultOpen))
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
    DrawNodeGizmos();
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
                    GEditor.shaderGlobals.accumulatedFrames = 0;
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
        ImGui::SeparatorText(PSI_DASHBOARD " Performance");
        const bool asyncComputeSupported =
            GContext->device->GetDeviceQueue(RHIDeviceQueueType::Graphics) !=
            GContext->device->GetDeviceQueue(RHIDeviceQueueType::Compute);
        bool asyncComputeEnabled = asyncComputeSupported && GEditor.asyncComputeEnabled;
        ImGui::BeginDisabled(!asyncComputeSupported);
        if (ImGui::Checkbox("Async Compute", &asyncComputeEnabled))
        {
            GEditor.asyncComputeEnabled = asyncComputeEnabled;
            GEditor.shaderGlobals.accumulatedFrames = 0;
            changed = true;
        }
        ImGui::EndDisabled();
        if (!asyncComputeSupported)
            ImGui::TextDisabled("A separate compute queue is not available.");
        ImGui::SeparatorText(PSI_PICTURE " Texture Sampling");
        if (ImGui::Checkbox("Anisotropic Filtering", &GEditor.rendererConfig.textureAnisoEnable))
        {
            changed = true;
            if (IsPathTracer(GEditor.rendererMode))
                GEditor.shaderGlobals.accumulatedFrames = 0;
        }
        {
            float anisoLevel = GEditor.rendererConfig.textureAnisoLevel;
            ImGui::BeginDisabled(!GEditor.rendererConfig.textureAnisoEnable);
            if (ImGui::SliderFloat("Anisotropy Level", &anisoLevel, 1.0f, 16.0f, "%.0f"))
            {
                GEditor.rendererConfig.textureAnisoLevel = anisoLevel;
                changed = true;
                if (IsPathTracer(GEditor.rendererMode))
                    GEditor.shaderGlobals.accumulatedFrames = 0;
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
                    GEditor.shaderGlobals.accumulatedFrames = 0;
            }
        }
        // Progressive-only telemetry & sample auto-pause.
        if (IsProgressive(GEditor.rendererMode))
        {
            ImGui::SeparatorText(PSI_SIGNAL " Stats");
            // Throughput: measure delta on the *frame* (dispatch) counter every ~250ms,
            // then derive samples/sec from frames/sec via the spp multiplier.
            //   frames  = accumulatedFrames / ptSamplesPerPixel      (dispatch count)
            //   samples = accumulatedFrames
            // i.e. one dispatch advances `spp` pixel-samples.
            static double   sLastSampleTime  = ImGui::GetTime();
            static uint32_t sLastFrameCount  = 0u;
            static float    sFramesPerSec    = 0.0f;
            uint32_t samplesPerDispatch = GEditor.shaderGlobals.ptSamplesPerPixel;
            uint32_t frameCount         = GEditor.shaderGlobals.accumulatedFrames / samplesPerDispatch;
            uint32_t completedSamples   = GEditor.shaderGlobals.accumulatedFrames;
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
                GEditor.shaderGlobals.accumulatedFrames = 0;
            };
            if (ImModalButton(PSI_ADJUST " 8sp", 1, 3))
            {
                GEditor.renderTask.autoPauseSampleLimit = 8;
                GEditor.shaderGlobals.accumulatedFrames = 0;
            };
            if (ImModalButton(PSI_ADJUST " NoLimit", 2, 3))
            {
                GEditor.renderTask.autoPauseSampleLimit = 0;
                GEditor.shaderGlobals.accumulatedFrames = 0;
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
                GEditor.shaderGlobals.accumulatedFrames = 0;
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
                GEditor.shaderGlobals.ptFireflyClamp = GContext->rendererSettings.energyClampOverride; // Default 1.0
                GEditor.shaderGlobals.accumulatedFrames = 0;
            }
            if (ImModalButton(PSI_BOLT " Fast", 1, 4))
            {
                GEditor.shaderGlobals.ptMaxBounces = 8;
                GEditor.shaderGlobals.ptFireflyClamp = GContext->rendererSettings.energyClampOverride; // Default 1.0
                GEditor.shaderGlobals.accumulatedFrames = 0;
            }
            if (ImModalButton(PSI_FIRE " Full", 2, 4))
            {
                GEditor.shaderGlobals.ptMaxBounces = 32;
                GEditor.shaderGlobals.ptFireflyClamp = GContext->rendererSettings.energyClampOverride; // Default 1.0
                GEditor.shaderGlobals.accumulatedFrames = 0;
            }
            if (ImModalButton(PSI_BEAKER " Über", 3, 4))
            {
                GEditor.shaderGlobals.ptMaxBounces = 100;
                GEditor.shaderGlobals.ptFireflyClamp = 100.0f;
                GEditor.shaderGlobals.accumulatedFrames = 0;
            }
            ImGui::SeparatorText(PSI_RANDOM " Ray Bounce");
            if (ImGui::SliderInt("Max Bounces", reinterpret_cast<int*>(&GEditor.shaderGlobals.ptMaxBounces), 0, 100,
                                 "%d", ImGuiSliderFlags_Logarithmic))
            {
                GEditor.shaderGlobals.accumulatedFrames = 0;
            }
            ImGui::SeparatorText(PSI_DASHBOARD " Performance");
            const bool serSupported = GContext->device->GetCapabilities().shaderExecutionReordering;
            bool serEnabled = serSupported && GEditor.rendererConfig.ptShaderExecutionReordering;
            ImGui::BeginDisabled(!serSupported);
            if (ImGui::Checkbox("Shader Execution Reordering", &serEnabled))
            {
                GEditor.rendererConfig.ptShaderExecutionReordering = serEnabled;
                GEditor.shaderGlobals.accumulatedFrames = 0;
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
                GEditor.shaderGlobals.accumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            if (ImGui::Checkbox("Energy Compensation", &GEditor.rendererConfig.energyCompensation))
            {
                GEditor.shaderGlobals.accumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            if (ImGui::Checkbox("Primary Light Visibility", &GEditor.rendererConfig.ptPrimaryLightVisibility))
            {
                GEditor.shaderGlobals.accumulatedFrames = 0;
            }
            if (ImGui::Checkbox("Emissive Triangle LBVH NEE", &GEditor.rendererConfig.ptLightBVHIncludeEmissiveClusters))
            {
                CommitSceneToGPU(true);
            }
            ImGui::SeparatorText(PSI_RANDOM " Sampling");
            const char* lightSamplerItems[] = {"Light BVH", "Uniform (Reference)"};
            int lightSampler = static_cast<int>(GEditor.rendererConfig.lightSamplerMode);
            if (ImGui::Combo("Light Sampler", &lightSampler, lightSamplerItems, 2))
            {
                GEditor.rendererConfig.lightSamplerMode = static_cast<LightSampler>(lightSampler);
                GEditor.shaderGlobals.accumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            ImGui::SliderFloat("Max Energy", &GEditor.shaderGlobals.ptFireflyClamp, 1.0f, 100.0f, "%.1f");
            if (ImGui::SliderFloat("Adaptive Threshold", &GEditor.shaderGlobals.adaptiveThreshold, 0.0f, 1.0f, "%.4f"))
                GEditor.shaderGlobals.accumulatedFrames = 0;
            if (ImGui::SliderInt("Adaptive Min Samples", reinterpret_cast<int*>(&GEditor.shaderGlobals.adaptiveMinSamples), 1, 256))
                GEditor.shaderGlobals.accumulatedFrames = 0;
            int spp = static_cast<int>(GEditor.shaderGlobals.ptSamplesPerPixel);
            if (ImGui::SliderInt("SPP", &spp, 1, 64, "%d", ImGuiSliderFlags_Logarithmic))
            {
                if (spp != static_cast<int>(GEditor.shaderGlobals.ptSamplesPerPixel))
                {
                    GEditor.shaderGlobals.ptSamplesPerPixel = static_cast<uint32_t>(std::max(1, spp));
                    GEditor.shaderGlobals.accumulatedFrames = 0;
                }
            }
            const char* samplerItems[] = {"PCG (Independent)", "Sobol (Quasi-Monte Carlo)"};
            int ptSampler = static_cast<int>(GEditor.rendererConfig.ptSampler);
            if (ImGui::Combo("Sampler", &ptSampler, samplerItems, 2))
            {
                GEditor.rendererConfig.ptSampler = static_cast<PTSampler>(ptSampler);
                GEditor.shaderGlobals.accumulatedFrames = 0;
                GEditor.state = FERunningEnter;
            }
            if (GEditor.rendererMode == ERendererMode::RealtimePT &&
                ImGui::CollapsingHeader("Denoiser (Lab)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::PushID("PTDenoise");
                auto& config = GEditor.rendererConfig;
                bool denoiserChanged = ImGui::Checkbox("Enable Denoiser", &config.ptDenoise);
                ImGui::BeginDisabled(!config.ptDenoise);
                denoiserChanged |= ImGui::Checkbox("Camera-only Temporal", &config.ptDenoiseTemporal);
                ImGui::BeginDisabled(!config.ptDenoiseTemporal);
                int diffuseHistory = static_cast<int>(config.ptDenoiseDiffuseHistory);
                if (ImGui::SliderInt("Diffuse History", &diffuseHistory, 1, 64))
                {
                    config.ptDenoiseDiffuseHistory = static_cast<uint32_t>(diffuseHistory);
                    denoiserChanged = true;
                }
                int specularHistory = static_cast<int>(config.ptDenoiseSpecularHistory);
                if (ImGui::SliderInt("Max Rough Specular History", &specularHistory, 1, 16))
                {
                    config.ptDenoiseSpecularHistory = static_cast<uint32_t>(specularHistory);
                    denoiserChanged = true;
                }
                denoiserChanged |= ImGui::SliderFloat("Temporal Depth Threshold", &config.ptDenoiseTemporalDepthThreshold,
                                                      0.001f, 0.1f, "%.3f", ImGuiSliderFlags_Logarithmic);
                denoiserChanged |= ImGui::SliderFloat("Temporal Normal Threshold", &config.ptDenoiseTemporalNormalThreshold,
                                                      0.5f, 0.999f, "%.3f");
                denoiserChanged |= ImGui::SliderFloat("History Clamp Sigma", &config.ptDenoiseHistoryClampSigma,
                                                      0.5f, 8.0f, "%.1f");
                if (ImGui::Button("Reset History (Next Trace)"))
                    ++GEditor.shaderGlobals.ptDenoiseHistoryEpoch;
                ImGui::EndDisabled();
                ImGui::SeparatorText("Spatial Filter");
                denoiserChanged |= ImGui::SliderFloat("Normal Power", &config.ptDenoiseNormalPower,
                                                      1.0f, 128.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
                denoiserChanged |= ImGui::SliderFloat("Relative Depth Sigma", &config.ptDenoiseRelativeDepthSigma,
                                                      0.001f, 0.1f, "%.3f", ImGuiSliderFlags_Logarithmic);
                denoiserChanged |= ImGui::SliderFloat("Albedo Sigma", &config.ptDenoiseAlbedoSigma,
                                                      0.01f, 1.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                denoiserChanged |= ImGui::SliderFloat("Luminance Sigma", &config.ptDenoiseLuminanceSigma,
                                                      0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
                ImGui::EndDisabled();
                ImGui::TextDisabled("Camera-only temporal resolve, then 3 a-trous passes.");
                ImGui::TextDisabled("Temporal requires static scenes and perspective pinhole cameras.");
                ImGui::TextDisabled("Background, emitters, and sharp/transmissive specular stay unfiltered.");
                ImGui::PopID();
                if (denoiserChanged)
                {
                    GEditor.shaderGlobals.accumulatedFrames = 0;
                    GEditor.state = FERunningEnter;
                }
            }
        }
        if (IsRaster(GEditor.rendererMode))
        {
            ImGui::SeparatorText(PSI_EYE_OPEN " Rasterizer");
            static float lodLogThreshold = 3;
            ImGui::SliderFloat("LOD ", &lodLogThreshold, 0, 8);
            GEditor.shaderGlobals.lodThreshold = std::pow(10.0f, -lodLogThreshold);
            ImGui::SeparatorText(PSI_DASHBOARD " Performance");
            changed |= ImGui::Checkbox("Force Texture LOD 0", &GEditor.rendererConfig.forceTextureLOD0);
            {
                const char* names[] = {"Overdraw", "Meshlet", "Matcap", "Light Tiles"};
                const ViewFlagsBits values[] = {ViewFlagsBits::Overdraw, ViewFlagsBits::Meshlet,
                                                ViewFlagsBits::Matcap, ViewFlagsBits::LightTiles};
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
                if (ImGui::CollapsingHeader("TAA", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::PushID("TAA");
                    changed |= ImGui::Checkbox("Enable TAA", &GEditor.rasterTAA);
                    ImGui::BeginDisabled(!GEditor.rasterTAA);
                    TAAConfig& taa = GEditor.rasterTAAConfig;
                    bool taaSettingsChanged = false;
                    taaSettingsChanged |=
                        ImGui::SliderFloat("History Weight", &taa.historyWeight, 0.0f, 0.99f, "%.2f");
                    taaSettingsChanged |=
                        ImGui::SliderFloat("Jitter Spread", &taa.jitterSpread, 0.0f, 2.0f, "%.2f");
                    int sampleCount = static_cast<int>(taa.sampleCount);
                    if (ImGui::SliderInt("Samples", &sampleCount, 2, 32))
                    {
                        taa.sampleCount = static_cast<uint32_t>(sampleCount);
                        taaSettingsChanged = true;
                    }
                    taaSettingsChanged |= ImGui::Checkbox("Neighborhood Clamp", &taa.neighborhoodClamp);
                    if (taaSettingsChanged)
                        GEditor.shaderGlobals.accumulatedFrames = 0u;
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
                    GEditor.shaderGlobals.accumulatedFrames = 0;
            }
        }
        {
            const char* items[] = {"White Base Color"};
            const MaterialFlagsBits values[] = {MaterialFlagsBits::DbgWhiteBaseColor};
            ImGui::SeparatorText(PSI_ADJUST " Material Debug");
            if (ImBitmaskOptionPicker(GEditor.rendererConfig.materialFlags, items, values, true /* solo */))
            {
                if (IsPathTracer(GEditor.rendererMode))
                    GEditor.shaderGlobals.accumulatedFrames = 0;
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
    DrawRenderGraphModal();
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
        uint32_t completedSamples = GEditor.shaderGlobals.accumulatedFrames;
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
    UpdateEditorCameraGlobals(renderer, GEditor.rendererConfig.renderExtent);
    GEditor.postprocessGlobals.camEV = GEditor.shaderGlobals.camEV;
    GEditor.postprocessGlobals.dbgShowOutline = GEditor.showImGui ? 1u : 0u;
    GEditor.postprocessGlobals.accumulatedFrames = GEditor.shaderGlobals.accumulatedFrames;
    GEditor.postprocessGlobals.fbWidth = GEditor.shaderGlobals.fbWidth;
    GEditor.postprocessGlobals.fbHeight = GEditor.shaderGlobals.fbHeight;
    GEditor.postprocessGlobals.viewLutIndex =
        Postprocess::ResolvePostprocessViewLutIndex(GEditor.viewLUTSdrHandle, GEditor.viewLUTHdrHandle, GContext->enableHDR);
    renderer->ExecuteFrame();
    if (!RHISwapchainResultMayPresent(EditorEndFrame(renderer, GContext->presenter)))
        RecreateEditorSwapchain();
    GEditor.shaderGlobals.accumulatedFrames +=
        IsRaster(GEditor.rendererMode) ? 1u : GEditor.shaderGlobals.ptSamplesPerPixel;

    bool timeLimitReached = GEditor.renderTask.targetTimeSeconds > 0 && elapsed >= GEditor.renderTask.targetTimeSeconds;
    bool sampleLimitReached = GEditor.renderTask.targetSamples > 0 && GEditor.shaderGlobals.accumulatedFrames >= targetFrames;

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

static void DrawNodeGizmos()
{
    if (!GEditor.gizmo.showImGuizmo || !GEditor.viewport.HasRect() || !IsNodeTransformable(GEditor.selectedNode))
        return;
    auto& scene = GEditor.Scene();
    mat4 world = scene.NodeWorldTransform(GEditor.selectedNode);
    ImVec2 const displaySize = GEditor.viewport.Size();
    ImGuizmo::BeginFrame();
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(GEditor.viewport.contentMin.x, GEditor.viewport.contentMin.y, displaySize.x, displaySize.y);
    auto const operation = static_cast<ImGuizmo::OPERATION>(GEditor.gizmo.op);
    if (ImGuizmo::Manipulate(&GEditor.camera.view[0][0], &GEditor.camera.proj[0][0], operation,
                             static_cast<ImGuizmo::MODE>(GEditor.gizmo.mode), &world[0][0], nullptr,
                             GizmoSnapForOperation(operation)))
    {
        if (scene.SetNodeWorldTransform(GEditor.selectedNode, world))
            CommitSceneToGPU(true);
    }
}

static void DrawViewportSelectionContextMenu()
{
    if (GEditor.applySelectionDoubleClickAction)
    {
        GEditor.applySelectionDoubleClickAction = false;
        int const light = SceneLightIndexFromId(GEditor.selectedLight);
        if (IsLightTransformable(light))
            AlignViewToLight(light);
        else
            FrameNode(GEditor.selectedNode);
    }

    if (GEditor.openSelectionContextMenu)
    {
        ImGui::OpenPopup("ViewportSelectionContextMenu");
        GEditor.openSelectionContextMenu = false;
    }

    ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
    if (!ImGui::BeginPopup("ViewportSelectionContextMenu"))
        return;

    bool const remove = SceneNodeIndexFromId(GEditor.selectedNode) >= 0 && DrawNodeActions(GEditor.selectedNode);
    ImGui::EndPopup();
    if (remove)
        DeleteSelectedNode();
}
