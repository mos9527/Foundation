#pragma once
#include <Bindings/ImGui.hpp>

struct ImProfilerHistogram
{
    const size_t capacity;
    float mean = 0, m2 = 0;
    size_t head = 0;
    Allocator* allocator;

    Vector<unsigned> samples;


    ImProfilerHistogram(size_t capacity, Allocator* alloc) : capacity(capacity), allocator(alloc), samples(alloc), sorted(alloc)
    {
        samples.reserve(capacity);
    };
    ImProfilerHistogram(ImProfilerHistogram const& other) :
        capacity(other.capacity),
        mean(other.mean),
        m2(other.m2),
        head(other.head),
        allocator(other.allocator),
        samples(other.allocator),
        sorted(other.allocator),
        mode(other.mode),
        median(other.median),
        maxCount(other.maxCount)
    {
        samples.reserve(capacity);
        samples.assign(other.samples.begin(), other.samples.end());
        sorted.assign(other.sorted.begin(), other.sorted.end());
    }
    ImProfilerHistogram(ImProfilerHistogram&& other) : ImProfilerHistogram(static_cast<ImProfilerHistogram const&>(other)) {}

    ImProfilerHistogram& operator=(ImProfilerHistogram const&) = delete;
    ImProfilerHistogram& operator=(ImProfilerHistogram&&) = delete;

    void push(unsigned data);
    void clear() { samples.clear(), mean = m2 = .0f, head = 0; }

    float variance() const { return samples.empty() ? 0.0f : m2 / samples.size(); }
    float stddev() const { return sqrtf(variance()); }

    Vector<unsigned> sorted;
    unsigned mode = 0, median = 0, maxCount = 0;
    // Side effects: updates sorted, mode, median, maxCount
    void bin(Vector<unsigned>& bins, size_t binCount, bool logOrLinear /* false/true */);
};
struct ImProfilerSample
{
    int id;

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
extern void ImFillText(const char* label, ImU32 col = 0xFFFFFFFF, int lineIndex = 0, int lineTotal = 1);

extern int ImProfilerAssignLanes(Span<ImProfilerSample>, Allocator*);
extern void ImProfilerDrawTimestampLabel(Span<const ImProfilerSample> samples, float resolution, int numLabels);
extern int ImProfilerDrawLane(Span<const ImProfilerSample>, int lane);
extern int ImProfilerDrawTable(Span<const ImProfilerSample>, float resolution /* ns = tick * resolution */);

extern void ImProfilerDrawHistogram(Vector<unsigned>& bins, ImProfilerHistogram const& histogram, size_t labelCount,
                                    float resolution /* ns = tick * resolution */, bool logOrLinear = true);

extern bool ImBitmaskOptionPicker(unsigned& value, const char** labels, const unsigned* masks, unsigned count, bool solo = false, int columns = 1);
template <size_t N>
bool ImBitmaskOptionPicker(unsigned& value, const char* (&labels)[N], const unsigned (&masks)[N], bool solo = false, int columns = 1)
{
    return ImBitmaskOptionPicker(value, labels, masks, N, solo, columns);
}
template <typename T, typename Mask, size_t N>
bool ImBitmaskOptionPicker(T& value, const char* (&labels)[N], const Mask (&masks)[N], bool solo = false,
                           int columns = 1)
{
    bool any = false;
    for (unsigned i = 0; i < N; i++)
    {
        bool selected = (value & masks[i]) != 0;
        if (ImGui::Checkbox(labels[i], &selected))
        {
            if (selected)
            {
                value |= masks[i];
                if (solo)
                {
                    for (unsigned j = 0; j < N; j++)
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
        if (i != N - 1 && (i + 1) % columns != 0)
            ImGui::SameLine();
    }
    return any;
}
float LinearToSRGB(float linear);
float SRGBToLinear(float srgb);
float3 LinearToSRGB(float3 linear);
float3 SRGBToLinear(float3 srgb);

// ColorEdit for scene-linear RGB: shows/edits sRGB display values, writes linear.
bool ImLinearColorEdit3(const char* label, float3& linearRgb);
bool ImLinearColorEdit4(const char* label, float4& linearRgba);
// Chromaticity RGB is display-encoded; power stays linear intensity.
bool ImHDRColorEdit(const char* label, float3& color, float& power, float maxScale = 100.0f);

// One row (track) drawn by ImTimeline. `muted` only dims the row visually.
struct ImTimelineRow
{
    const char* label = nullptr;
    bool muted = false;
};

// One draggable/resizable [start, end) block placed on an ImTimeline row. Callers own the backing
// data; ImTimeline mutates `start`/`end` directly while a strip is being dragged/resized, the same
// way ImGui::DragFloat mutates its target in place.
struct ImTimelineStrip
{
    int row = 0; // index into the ImTimelineRow span this strip is drawn on
    float start = 0.0f;
    float end = 0.0f;
    ImU32 color = 0;
    const char* label = nullptr;
    bool selected = false;
};

// Per-frame interaction result from ImTimeline, so the caller can react (select, mark dirty, open
// its own context menu popups, ...) without the widget knowing anything about the caller's data
// model. ImTimeline never opens a popup itself; it only reports where a right-click landed.
struct ImTimelineResult
{
    int clickedRow = -1;    // >= 0 if a row's label area was left-clicked this frame
    int muteToggledRow = -1; // >= 0 if a row's label was clicked to toggle its mute state this frame
    int clickedStrip = -1;  // >= 0 if a strip was left-clicked and/or is being interacted with
    bool stripsChanged = false; // true if `clickedStrip`'s start/end was mutated by a drag this frame
    bool scrubbed = false;      // true if the ruler was clicked/dragged, updating `playhead`
    int rightClickedRow = -1;    // >= 0 if a row's label area was right-clicked this frame
    int rightClickedStrip = -1;  // >= 0 if a strip was right-clicked this frame
    bool rightClickedBackground = false; // true if empty ruler/canvas space was right-clicked
};

// A horizontal ruler + N rows of draggable/resizable strips (a minimal NLA/sequencer-style track
// view), laid out as a 2-column table so the row labels never scroll under the strips and the
// widget never needs its own vertical scrollbar (it's exactly as tall as its rows).
//
// `length` sets the timeline extent in caller units (e.g. seconds). `playhead` is scrubbed in place
// by clicking/dragging the ruler. `pixelsPerSecond`/`scrollX` are zoom/pan state owned by the caller
// (persist them across frames); hovering the widget scrolls the wheel to zoom (keeping time under
// the cursor fixed), and Ctrl+wheel pans horizontally instead.
//
// Right-clicks are only reported via `ImTimelineResult`, never turned into a popup here: the caller
// owns what actions make sense for its data (e.g. "Add Strip", "Remove Track") and should respond
// with its own plain `ImGui::OpenPopup`/`BeginPopup` calls after this returns.
ImTimelineResult ImTimeline(const char* strId, Span<const ImTimelineRow> rows, Span<ImTimelineStrip> strips,
                            float length, float& playhead, float& pixelsPerSecond, float& scrollX);

// Generate linear, monotonous ints of [0, count - 1] at interval of intervalMS
inline int ImBlink(int intervalMS, int count)
{
    size_t time = ImGui::GetTime() * 1000;
    time = time % (intervalMS * count);
    return time / intervalMS;
}

// Generate linear, monotonous float in range of [0, 1] at interval of intervalMS
inline float ImBlinkF(float intervalMS)
{
    float time = ImGui::GetTime();
    intervalMS /= 1000.0f;
    time = fmod(time, intervalMS);
    return time / intervalMS;
}

// CSS linear easing function on x of range [0,1]
constexpr inline float ImEaseLinear(float x) { return x; }

// CSS easeInOutCubic easing function on x of range [0,1]
constexpr inline float ImEaseInOutCubic(float x)
{
    return x < 0.5f ? 4 * pow(x, 3.0f) : 1.0f - pow(-2.0f * x + 2.0f, 3.0f) / 2.0f;
}

using ImEaseFunction = float (*)(float);
