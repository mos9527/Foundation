// Only useful if you're manipulating the DrawList which has positions
// that are _NOT_ window local
Tuple<ImVec2, ImVec2, ImDrawList*> ImWindowDrawOffsetRegionList()
{
    ImVec2 offset = ImGui::GetCursorScreenPos();
    ImVec2 region = ImGui::GetContentRegionAvail();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    return {offset, region, drawList};
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
        float u = i / float(numLabels);
        float x = offset.x + region.x * u;
        cmd->AddLine({x, offset.y}, {x, offset.y + height}, IM_COL32(200, 200, 200, 255));
        String label = fmt::format("{:.3f} ms", duration * u * resolution * 1e-6f);
        ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        cmd->AddText(font, height * 0.75f, {x - textSize.x, offset.y}, IM_COL32(255, 255, 255, 255),
                     label.c_str());
    }
    ImGui::Dummy({region.x, height + style.ItemSpacing.y});
}

void ImProfilerDrawLane(Span<const ImProfilerSample> samples, float resolution, int lane)
{
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
        if (mousePos.x >= rectMin.x && mousePos.x <= rectMax.x && mousePos.y >= rectMin.y &&
            mousePos.y <= rectMax.y)
        {
            ImGui::SetTooltip("%s\n%.3f ms", sample.label.c_str(),
                              (sample.endTick - sample.startTick) * resolution * 1e-6f);
        }
    }
    ImGui::Dummy({region.x, height + style.ItemSpacing.y});
}
void ImProfilerDrawTable(Span<const ImProfilerSample> samples, float resolution, int lane)
{
    auto [offset, region, cmd] = ImWindowDrawOffsetRegionList();
    float duration = samples.back().endTick - samples.front().startTick;
    ImGui::PushID(lane);
    if (ImGui::BeginTable("##", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("Duration");
        ImGui::TableSetupColumn("%");
        ImGui::TableHeadersRow();
        for (auto const& sample : samples)
        {
            if (sample.lane != lane)
                continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(sample.label.c_str());
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
    ImGui::PopID();
}