#include "ImGui.hpp"
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
    // Pop ring buffer
    if (samples.size() >= capacity)
    {
        unsigned old = samples.front();
        samples.pop_front();
        // Welford mean/std remove
        float d = old - mean;
        mean -= d / samples.size();
        m2 -= d * (old - mean);
    }
    // Welford mean/std update
    float d = data - mean;
    mean += d / (samples.size() + 1);
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
    const float logMin = std::log10f(min);
    const float binSize = (std::log10f(max) - logMin) / binCount;
    return std::pow(10.0f, logMin + binSize * binIndex);
}
void ImProfilerHistogram::bin(Vector<unsigned>& bins, size_t binCount, bool logOrLinear)
{
    sorted.resize(samples.size());
    Ranges::copy(samples, sorted.begin());
    Ranges::sort(sorted);
    size_t min = sorted.front(), max = sorted.back();
    median = sorted[sorted.size() / 2];
    bins.clear(), bins.resize(binCount, 0);
    maxCount = 0;
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

int ImProfilerAssignLanes(Span<ImProfilerSample> samples)
{
    std::sort(samples.begin(), samples.end());
    // Partition into lanes - work has chance to overlap on the GPU.
    Map<int, size_t> Q(GLOBAL_ALLOC);
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
        String label = fmt::format("{:.3f} ms", duration * u * resolution * 1e-6f);
        ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        cmd->AddText(font, height * 0.75f, {x - textSize.x, offset.y}, IM_COL32(255, 255, 255, 255), label.c_str());
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
            auto pstr = fmt::format("{:.3f}%", perc * 100.0f);
            ImGui::ProgressBar(perc, ImVec2(region.x * 0.25f, 0), pstr.c_str());
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
        String label = fmt::format("{:.3f} ms", ts * resolution * 1e-6f);
        cmd->AddText(font, labelHeight * 0.75f, {x, offset.y + labelHeight}, IM_COL32(255, 255, 255, 255),
                     label.c_str());
    }
    ImGui::Dummy({region.x, labelHeight + style.ItemSpacing.y});
}
bool ImBitmaskOptionPicker(unsigned& value, const char** labels, const unsigned* masks, unsigned count, bool solo)
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
        if (i != count - 1)
            ImGui::SameLine();
    }
    return any;
}
