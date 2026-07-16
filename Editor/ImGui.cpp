#include "ImGui.hpp"
#include <Fonts/PlexSansIcon.h>
#include <cmath>
#include <cstdio>
// Only useful if you're manipulating the DrawList which has positions
// that are _NOT_ window local
Tuple<ImVec2, ImVec2, ImDrawList*> ImWindowDrawOffsetRegionList()
{
    ImVec2 offset = ImGui::GetCursorScreenPos();
    ImVec2 region = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    return {offset, region, drawList};
}
void ImProfilerHistogram::push(unsigned data)
{
    if (!capacity)
        return;

    const size_t count = samples.size();
    if (samples.size() >= capacity)
    {
        unsigned old = samples[head];
        if (count > 1)
        {
            float oldMean = mean;
            mean = (mean * count - old) / (count - 1);
            m2 -= (old - oldMean) * (old - mean);
        }
        else
        {
            mean = m2 = 0.0f;
        }

        float d = data - mean;
        mean += d / count;
        m2 += d * (data - mean);
        samples[head] = data;
        head = (head + 1) % capacity;
        return;
    }

    float d = data - mean;
    mean += d / (count + 1);
    m2 += d * (data - mean);
    samples.push_back(data);
}
const size_t ImProfilerTimestampLinear(size_t min, size_t max, size_t binCount, size_t binIndex)
{
    const size_t range = max - min + 1;
    const size_t binSize = (range + binCount - 1) / binCount;
    return min + std::min(binSize * binIndex, range);
}

const size_t ImProfilerTimestampLog(size_t min, size_t max, size_t binCount, size_t binIndex)
{
    const float logMin = std::log10(min);
    const float binSize = (std::log10(max) - logMin) / binCount;
    return std::pow(10.0f, logMin + binSize * binIndex);
}
void ImProfilerHistogram::bin(Vector<unsigned>& bins, size_t binCount, bool logOrLinear)
{
    bins.clear(), bins.resize(binCount, 0);
    maxCount = mode = median = 0;
    if (samples.empty() || !binCount)
        return;

    sorted.resize(samples.size());
    for (size_t i = 0; i < samples.size(); i++)
        sorted[i] = samples[(head + i) % samples.size()];
    Ranges::sort(sorted);
    size_t min = sorted.front(), max = sorted.back();
    median = sorted[sorted.size() / 2];
    auto it0 = sorted.begin();
    for (size_t b = 1; b <= binCount; b++)
    {
        size_t end;
        if (logOrLinear)
            end = ImProfilerTimestampLinear(min, max, binCount, b);
        else
            end = ImProfilerTimestampLog(min, max, binCount, b);
        auto it1 = std::lower_bound(it0, sorted.end(), end);
        bins[b - 1] = it1 - it0;
        maxCount = std::max(maxCount, bins[b - 1]);
        if (maxCount == bins[b - 1])
            mode = end;
        it0 = it1;
    }
}
bool ImModalButton(const char* label, int lineIndex, int lineTotal)
{
    auto& style = ImGui::GetStyle();
    float padding = style.FramePadding.x;
    float width = ImGui::GetContentRegionAvail().x / lineTotal;
    if (lineIndex)
        ImGui::SameLine();
    return ImGui::Button(label, lineTotal > 1 ? ImVec2{width - padding, 0} : ImVec2{width, 0});
}

void ImFillText(const char* label, ImU32 col, int lineIndex, int lineTotal)
{
    auto& style   = ImGui::GetStyle();
    float padding = style.FramePadding.x;
    float availX  = ImGui::GetContentRegionAvail().x;
    float availY  = ImGui::GetContentRegionAvail().y;
    float colWidth = lineTotal > 1 ? (availX / lineTotal - padding) : availX;

    auto* font = ImGui::GetFont();
    ImVec2 textSize = ImGui::CalcTextSize(label);
    float scale     = (textSize.x > 0.0f) ? (colWidth / textSize.x) : 1.0f;
    float fontSize  = ImGui::GetFontSize() * scale;
    float height    = textSize.y * scale;

    if (lineIndex)
        ImGui::SameLine();

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float yOffset = (availY - height) * 0.5f;
    ImVec2 textPos = {cursor.x, cursor.y + std::max(0.0f, yOffset)};
    
    ImGui::GetWindowDrawList()->AddText(font, fontSize, textPos, col, label);
    ImGui::Dummy(ImVec2{colWidth, height});
}

int ImProfilerAssignLanes(Span<ImProfilerSample> samples, Allocator* alloc)
{
    std::sort(samples.begin(), samples.end());
    // Partition into lanes - work has chance to overlap on the GPU.
    Map<int, size_t> Q(alloc);
    for (auto& sample : samples)
    {
        for (int lane = 0;; lane++)
        {
            if (Q[lane] <= sample.startTick)
            {
                sample.lane = lane, Q[lane] = sample.endTick;
                break;
            }
        }
    }
    return Q.size();
}
void ImProfilerDrawTimestampLabel(Span<const ImProfilerSample> samples, float resolution, int numLabels)
{
    auto& style = ImGui::GetStyle();
    auto* font = ImGui::GetFont();
    auto [offset, region, cmd] = ImWindowDrawOffsetRegionList();
    float height = style.ScrollbarSize, duration = samples.back().endTick - samples.front().startTick;
    for (int i = 0; i <= numLabels; i++)
    {
        float u = i / static_cast<float>(numLabels);
        float x = offset.x + region.x * u;
        cmd->AddLine({x, offset.y}, {x, offset.y + height}, IM_COL32(200, 200, 200, 255));
        char label[32];
        std::snprintf(label, sizeof(label), "%.3f ms", duration * u * resolution * 1e-6f);
        ImVec2 textSize = ImGui::CalcTextSize(label);
        cmd->AddText(font, height * 0.75f, {x - textSize.x, offset.y}, IM_COL32(255, 255, 255, 255), label);
    }
    ImGui::Dummy({region.x, height + style.ItemSpacing.y});
}

int ImProfilerDrawLane(Span<const ImProfilerSample> samples, int lane)
{
    int selected = -1;
    auto& style = ImGui::GetStyle();
    auto* font = ImGui::GetFont();
    auto [offset, region, cmd] = ImWindowDrawOffsetRegionList();
    float height = style.ScrollbarSize, duration = samples.back().endTick - samples.front().startTick;
    for (auto const& sample : samples)
    {
        if (sample.lane != lane)
            continue;
        float u = (sample.startTick - samples.front().startTick) / duration;
        float v = (sample.endTick - samples.front().startTick) / duration;
        ImVec2 rectMin{offset.x + region.x * u, offset.y + style.ItemSpacing.y};
        ImVec2 rectMax{offset.x + region.x * v, offset.y + height + style.ItemSpacing.y};
        cmd->AddRectFilled(rectMin, rectMax, sample.color);
        float wrapWidth = rectMax.x - rectMin.x;
        const char* textEnd = sample.label.c_str();
        while (ImGui::CalcTextSize(sample.label.c_str(), textEnd + 1).x < wrapWidth && *textEnd)
            textEnd++;
        cmd->AddText(font, height * 0.85f, rectMin, IM_COL32(255, 255, 255, 255), sample.label.c_str(), textEnd,
                     wrapWidth);
        // Hover tooltip
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        if (mousePos.x >= rectMin.x && mousePos.x <= rectMax.x && mousePos.y >= rectMin.y && mousePos.y <= rectMax.y)
            selected = sample.id;
    }
    ImGui::Dummy({region.x, height + style.ItemSpacing.y});
    return selected;
}
int ImProfilerDrawTable(Span<const ImProfilerSample> samples, float resolution)
{
    int selected = -1;
    auto [offset, region, cmd] = ImWindowDrawOffsetRegionList();
    float duration = samples.back().endTick - samples.front().startTick;
    if (ImGui::BeginTable("##", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("Duration");
        ImGui::TableSetupColumn("%");
        ImGui::TableHeadersRow();
        for (auto const& sample : samples)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(sample.label.c_str());
            // Mouse hover
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            ImVec2 rowMin = ImGui::GetItemRectMin();
            ImVec2 rowMax = ImGui::GetItemRectMax();
            if (mousePos.x >= rowMin.x && mousePos.x <= rowMax.x && mousePos.y >= rowMin.y && mousePos.y <= rowMax.y)
                selected = sample.id;
            ImGui::TableSetColumnIndex(1);
            float durMS = (sample.endTick - sample.startTick) * resolution * 1e-6f;
            ImGui::Text("%.3f ms", durMS);
            ImGui::TableSetColumnIndex(2);
            float perc = (sample.endTick - sample.startTick) / duration;
            char pstr[32];
            std::snprintf(pstr, sizeof(pstr), "%.3f%%", perc * 100.0f);
            ImGui::ProgressBar(perc, ImVec2(region.x * 0.25f, 0), pstr);
        }
        ImGui::EndTable();
    }
    return selected;
}

void ImProfilerDrawHistogram(Vector<unsigned>& bins, ImProfilerHistogram const& histogram, size_t labelCount,
                             float resolution, bool logOrLinear)
{
    unsigned min = histogram.sorted.front(), max = histogram.sorted.back(), maxCount = histogram.maxCount;
    auto Getter = [](void* data, int idx) -> float
    {
        auto& bins = *static_cast<const Vector<unsigned>*>(data);
        return bins[idx];
    };
    {
        auto [offset, region, cmd] = ImWindowDrawOffsetRegionList();
        ImGui::PlotHistogram("##Histogram", Getter, &bins, bins.size(), 0, nullptr, 0.0f, static_cast<float>(maxCount),
                             ImVec2(region.x, region.y * 0.75f));
    }
    // X Axis labels
    auto& style = ImGui::GetStyle();
    auto* font = ImGui::GetFont();
    auto [offset, region, cmd] = ImWindowDrawOffsetRegionList();
    float labelHeight = style.ScrollbarSize;
    for (size_t i = 0; i < labelCount; i++)
    {
        size_t binIndex = i * bins.size() / labelCount;
        size_t ts;
        if (logOrLinear)
            ts = ImProfilerTimestampLinear(min, max, bins.size(), binIndex);
        else
            ts = ImProfilerTimestampLog(min, max, bins.size(), binIndex);
        float u = (ts - min) / static_cast<float>(max - min);
        float x = offset.x + region.x * u;
        cmd->AddLine({x, offset.y}, {x, offset.y + labelHeight * 0.5f}, IM_COL32(200, 200, 200, 255));
        char label[32];
        std::snprintf(label, sizeof(label), "%.3f ms", ts * resolution * 1e-6f);
        cmd->AddText(font, labelHeight * 0.75f, {x, offset.y + labelHeight}, IM_COL32(255, 255, 255, 255),
                     label);
    }
    ImGui::Dummy({region.x, labelHeight + style.ItemSpacing.y});
}
bool ImBitmaskOptionPicker(unsigned& value, const char** labels, const unsigned* masks, unsigned count, bool solo,
                           int columns)
{
    bool any = false;
    for (unsigned i = 0; i < count; i++)
    {
        bool selected = (value & masks[i]) != 0;
        if (ImGui::Checkbox(labels[i], &selected))
        {
            if (selected)
            {
                value |= masks[i];
                if (solo)
                {
                    for (unsigned j = 0; j < count; j++)
                    {
                        if (j != i)
                            value &= ~masks[j];
                    }
                }
            }
            else
                value &= ~masks[i];
            any = true;
        }
        if (i != count - 1 && (i + 1) % columns != 0)
            ImGui::SameLine();
    }
    return any;
}
ImTimelineResult ImTimeline(const char* strId, Span<const ImTimelineRow> rows, Span<ImTimelineStrip> strips,
                            float length, float& playhead, float& pixelsPerSecond, float& scrollX)
{
    ImTimelineResult result;
    if (rows.empty())
        return result;

    constexpr float kLabelWidth = 130.0f;
    constexpr float kRulerHeight = 22.0f;
    constexpr float kRowHeight = 32.0f;
    constexpr float kHandleWidth = 7.0f;
    constexpr float kStripPadY = 4.0f; // vertical inset of the strip block within its row

    ImGui::PushID(strId);

    float const availWidth = ImGui::GetContentRegionAvail().x;
    float const canvasWidth = std::max(availWidth - kLabelWidth, 1.0f);
    float const totalHeight = kRulerHeight + static_cast<float>(rows.size()) * kRowHeight;
    length = std::max(length, 1.0f);
    pixelsPerSecond = std::clamp(pixelsPerSecond, 4.0f, 400.0f);
    float const maxScroll = std::max(length * pixelsPerSecond - canvasWidth, 0.0f);
    scrollX = std::clamp(scrollX, 0.0f, maxScroll);

    // Mouse wheel over the widget: plain wheel zooms (keeping the time under the cursor fixed),
    // Ctrl+wheel pans horizontally instead.
    ImVec2 const origin = ImGui::GetCursorScreenPos();
    ImVec2 const canvasMin(origin.x + kLabelWidth, origin.y);
    ImVec2 const canvasMax(origin.x + availWidth, origin.y + totalHeight);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(canvasMin, canvasMax))
    {
        float const wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
        {
            if (ImGui::GetIO().KeyCtrl)
            {
                scrollX = std::clamp(scrollX - wheel * 80.0f, 0.0f, maxScroll);
            }
            else
            {
                float const cursorX = ImGui::GetIO().MousePos.x - canvasMin.x;
                float const timeAtCursor = (scrollX + cursorX) / pixelsPerSecond;
                pixelsPerSecond = std::clamp(pixelsPerSecond * (wheel > 0.0f ? 1.15f : 1.0f / 1.15f), 4.0f, 400.0f);
                float const newMaxScroll = std::max(length * pixelsPerSecond - canvasWidth, 0.0f);
                scrollX = std::clamp(timeAtCursor * pixelsPerSecond - cursorX, 0.0f, newMaxScroll);
            }
        }
    }

    // Middle-mouse drag pans horizontally. Gate on the drag's origin (not the current cursor) so a
    // pan started over the canvas keeps working even as the mouse strays past its edges.
    ImVec2 const panOrigin = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Middle];
    bool const panStartedInCanvas = panOrigin.x >= canvasMin.x && panOrigin.x <= canvasMax.x &&
                                     panOrigin.y >= canvasMin.y && panOrigin.y <= canvasMax.y;
    if (panStartedInCanvas && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        float const panMax = std::max(length * pixelsPerSecond - canvasWidth, 0.0f);
        scrollX = std::clamp(scrollX - ImGui::GetIO().MouseDelta.x, 0.0f, panMax);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    auto timeToX = [&](float t) { return canvasMin.x + t * pixelsPerSecond - scrollX; };

    // Table just for layout: column 0 (labels) and column 1 (canvas) are always aligned per row,
    // so strips can never render on top of a label. All actual drawing/dragging happens in a
    // second overlay pass below, using screen coordinates computed from `origin` (fixed row
    // heights make this exact); interactive items still live inline here for correct hit ordering.
    // Cell padding must be zero so each table row is exactly kRulerHeight/kRowHeight tall; otherwise
    // the rows drift taller than the overlay assumes and strips no longer line up with their labels.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::BeginTable("##table", 2, ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed, kLabelWidth);
        ImGui::TableSetupColumn("##canvas", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow(ImGuiTableRowFlags_None, kRulerHeight);
        ImGui::TableSetColumnIndex(0);
        ImGui::Dummy(ImVec2(kLabelWidth, kRulerHeight));
        ImGui::TableSetColumnIndex(1);
        ImGui::InvisibleButton("##scrub", ImVec2(canvasWidth, kRulerHeight));
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // wheel over the canvas zooms, never scrolls the window
        if (ImGui::IsItemActive())
        {
            playhead = std::clamp((ImGui::GetIO().MousePos.x - canvasMin.x + scrollX) / pixelsPerSecond, 0.0f, length);
            result.scrubbed = true;
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
            result.rightClickedBackground = true;

        for (int ri = 0; ri < static_cast<int>(rows.size()); ++ri)
        {
            ImGui::PushID(ri);
            ImTimelineRow const& row = rows[ri];
            ImGui::TableNextRow(ImGuiTableRowFlags_None, kRowHeight);

            ImGui::TableSetColumnIndex(0);
            // A speaker icon reflects mute state; left-clicking the whole title row toggles it.
            char rowLabel[160];
            snprintf(rowLabel, sizeof(rowLabel), "%s %s", row.muted ? PSI_VOLUME_OFF : PSI_VOLUME_UP,
                     row.label ? row.label : "Track");
            ImGui::PushStyleColor(ImGuiCol_Text,
                                   ImGui::GetColorU32(row.muted ? ImGuiCol_TextDisabled : ImGuiCol_Text));
            ImGui::Selectable(rowLabel, false, 0, ImVec2(0, kRowHeight));
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Click to %s", row.muted ? "unmute" : "mute");
            if (ImGui::IsItemClicked())
            {
                result.clickedRow = ri;
                result.muteToggledRow = ri;
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                result.rightClickedRow = ri;

            ImGui::TableSetColumnIndex(1);
            // Allow overlap so the strip buttons drawn on top of this row (submitted later) can win
            // hover/click; without this the row background swallows every strip interaction.
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##row_bg", ImVec2(canvasWidth, kRowHeight));
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                result.rightClickedRow = ri;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    // The overlay pass below repositions the cursor with SetCursorScreenPos for each strip; capture
    // the correct post-table position now and restore it before returning so following widgets flow
    // beneath the timeline instead of being drawn on top of it.
    ImVec2 const cursorBelow = ImGui::GetCursorScreenPos();

    // Overlay pass: ruler ticks, playhead, and strips, drawn/interacted with in absolute screen
    // coordinates derived from `origin` + fixed row heights (matching the table layout above
    // exactly), clipped to the canvas so scrolled-off content never bleeds into the label column.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(canvasMin, canvasMax, true);

    float const tickStep = pixelsPerSecond >= 30.0f ? 1.0f : (pixelsPerSecond >= 12.0f ? 2.0f : 5.0f);
    float const firstTick = std::max(std::floor(scrollX / pixelsPerSecond / tickStep) * tickStep, 0.0f);
    for (float t = firstTick; t <= length; t += tickStep)
    {
        float const x = timeToX(t);
        dl->AddLine(ImVec2(x, canvasMin.y + kRulerHeight * 0.5f), ImVec2(x, canvasMax.y),
                    ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.35f));
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0fs", t);
        dl->AddText(ImVec2(x + 2.0f, canvasMin.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), buf);
    }

    float const dt = ImGui::GetIO().MouseDelta.x / pixelsPerSecond;
    for (int si = 0; si < static_cast<int>(strips.size()); ++si)
    {
        ImTimelineStrip& strip = strips[si];
        if (strip.row < 0 || strip.row >= static_cast<int>(rows.size()))
            continue;
        float const x0 = timeToX(strip.start);
        float const x1 = timeToX(strip.end);
        if (x1 < canvasMin.x || x0 > canvasMax.x) // fully scrolled off-screen; skip entirely
            continue;

        ImGui::PushID(si + 100000);
        float const rowY = canvasMin.y + kRulerHeight + static_cast<float>(strip.row) * kRowHeight;
        ImVec2 const pMin(x0, rowY + kStripPadY), pMax(x1, rowY + kRowHeight - kStripPadY);
        {
            auto scaleColor = [](ImU32 c, float f) -> ImU32
            {
                ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
                v.x = std::min(v.x * f, 1.0f), v.y = std::min(v.y * f, 1.0f), v.z = std::min(v.z * f, 1.0f);
                return ImGui::ColorConvertFloat4ToU32(v);
            };
            dl->AddRectSDF(pMin, pMax, ImGuiSdfPreset_DropShadow, 4.0f, IM_COL32(0, 0, 0, 110), 0, 3.0f);
            dl->AddRectSDF(pMin, pMax, ImGuiSdfPreset_Gloss, 4.0f, scaleColor(strip.color, 1.18f),
                           scaleColor(strip.color, 0.80f));
        }
        dl->AddRect(pMin, pMax, strip.selected ? IM_COL32(255, 220, 90, 255) : IM_COL32(0, 0, 0, 120), 4.0f, 0,
                    strip.selected ? 2.0f : 1.0f);
        if (strip.label)
        {
            // Vertically center the label within the strip and inset it horizontally so it isn't
            // truncated flush against the rounded corners.
            float const textY = rowY + (kRowHeight - ImGui::GetTextLineHeight()) * 0.5f;
            dl->PushClipRect(ImVec2(pMin.x + 4.0f, pMin.y), pMax, true);
            dl->AddText(ImVec2(pMin.x + 6.0f, textY), IM_COL32(0, 0, 0, 220), strip.label);
            dl->PopClipRect();
        }

        auto handleRightClick = [&]
        {
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                result.rightClickedStrip = si;
        };

        // Only carve out separate resize handles when the strip is wide enough to comfortably split;
        // narrow strips get one full-width body hit area so they don't become impossible to grab.
        float const stripW = x1 - x0;
        bool const wide = stripW > kHandleWidth * 4.0f;

        // Body: drag to move (start/end translate together, preserving length). Submitted first so
        // the edge handles below win the overlapping edge pixels for hover/hit priority.
        float const bodyX = wide ? x0 + kHandleWidth : x0;
        float const bodyW = std::max(wide ? stripW - kHandleWidth * 2.0f : stripW, 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(bodyX, rowY));
        ImGui::SetNextItemAllowOverlap(); // let the edge handles below win their overlapping pixels
        ImGui::InvisibleButton("##body", ImVec2(bodyW, kRowHeight));
        ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
        if (ImGui::IsItemClicked())
            result.clickedStrip = si;
        handleRightClick();
        if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0.0f)
        {
            float const len = strip.end - strip.start;
            strip.start = std::max(0.0f, strip.start + dt);
            strip.end = strip.start + len;
            result.clickedStrip = si;
            result.stripsChanged = true;
        }

        if (wide)
        {
            // Edges: drag to trim start/end independently.
            ImGui::SetCursorScreenPos(ImVec2(x0 - kHandleWidth * 0.5f, rowY));
            ImGui::InvisibleButton("##left", ImVec2(kHandleWidth * 2.0f, kRowHeight));
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemClicked())
                result.clickedStrip = si;
            handleRightClick();
            if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0.0f)
            {
                strip.start = std::clamp(strip.start + dt, 0.0f, strip.end - 0.05f);
                result.clickedStrip = si;
                result.stripsChanged = true;
            }

            ImGui::SetCursorScreenPos(ImVec2(x1 - kHandleWidth * 1.5f, rowY));
            ImGui::InvisibleButton("##right", ImVec2(kHandleWidth * 2.0f, kRowHeight));
            ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
            if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (ImGui::IsItemClicked())
                result.clickedStrip = si;
            handleRightClick();
            if (ImGui::IsItemActive() && ImGui::GetIO().MouseDelta.x != 0.0f)
            {
                strip.end = std::max(strip.end + dt, strip.start + 0.05f);
                result.clickedStrip = si;
                result.stripsChanged = true;
            }
        }
        ImGui::PopID();
    }

    // Playhead, on top of everything else.
    float const px = timeToX(std::clamp(playhead, 0.0f, length));
    dl->AddLine(ImVec2(px, canvasMin.y), ImVec2(px, canvasMax.y), IM_COL32(255, 80, 80, 255), 2.0f);

    dl->PopClipRect();
    ImGui::SetCursorScreenPos(cursorBelow);
    ImGui::PopID();
    return result;
}

float LinearToSRGB(float linear)
{
    linear = std::max(linear, 0.0f);
    return (linear <= 0.0031308f) ? (linear * 12.92f) : (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
}

float SRGBToLinear(float srgb)
{
    srgb = std::max(srgb, 0.0f);
    return (srgb <= 0.04045f) ? (srgb / 12.92f) : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

float3 LinearToSRGB(float3 linear)
{
    return float3{LinearToSRGB(linear.x), LinearToSRGB(linear.y), LinearToSRGB(linear.z)};
}

float3 SRGBToLinear(float3 srgb)
{
    return float3{SRGBToLinear(srgb.x), SRGBToLinear(srgb.y), SRGBToLinear(srgb.z)};
}

bool ImLinearColorEdit3(const char* label, float3& linearRgb)
{
    float3 display = LinearToSRGB(linearRgb);
    if (!ImGui::ColorEdit3(label, &display.x, ImGuiColorEditFlags_Float))
        return false;
    linearRgb = SRGBToLinear(display);
    return true;
}

bool ImLinearColorEdit4(const char* label, float4& linearRgba)
{
    float display[4] = {
        LinearToSRGB(linearRgba.x),
        LinearToSRGB(linearRgba.y),
        LinearToSRGB(linearRgba.z),
        linearRgba.w,
    };
    if (!ImGui::ColorEdit4(label, display, ImGuiColorEditFlags_Float))
        return false;
    linearRgba.x = SRGBToLinear(display[0]);
    linearRgba.y = SRGBToLinear(display[1]);
    linearRgba.z = SRGBToLinear(display[2]);
    linearRgba.w = display[3];
    return true;
}

bool ImHDRColorEdit(const char* label, float3& color, float& power, float maxScale)
{
    bool changed = false;
    ImGui::PushID(label);

    changed |= ImLinearColorEdit3(label, color);
    char sliderLabel[128];
    snprintf(sliderLabel, sizeof(sliderLabel), "%s Power", label);
    changed |= ImGui::SliderFloat(sliderLabel, &power, 0.0f, maxScale, "%.3f", ImGuiSliderFlags_Logarithmic);

    ImGui::PopID();
    return changed;
}
