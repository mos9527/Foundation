#pragma once
#include <Bindings/ImGui.hpp>

struct ImProfilerSample
{
    size_t startTick;
    size_t endTick;
    int lane{-1};

    String label;
    ImColor color;
    constexpr bool operator<(const ImProfilerSample& o) const
    {
        return Pair{startTick, endTick} < Pair{o.startTick, o.endTick};
    }
};
extern bool ImModalButton(const char* label, int lineIndex = 0, int lineTotal = 1);

extern int ImProfilerAssignLanes(Span<ImProfilerSample>);
extern void ImProfilerDrawTimestampLabel(Span<const ImProfilerSample>, float resolution /* ns = tick * resolution */, int numLabels = 8);
extern void ImProfilerDrawLane(Span<const ImProfilerSample>, float resolution /* ns = tick * resolution */, int lane);
extern void ImProfilerDrawTable(Span<const ImProfilerSample>, float resolution /* ns = tick * resolution */, int lane);
