#pragma once
#include <Bindings/ImGui.hpp>

struct ImProfilerHistogram
{
    const size_t capacity;
    float mean = 0, m2 = 0;

    Deque<unsigned> samples;


    ImProfilerHistogram(size_t capacity, Allocator* alloc) : capacity(capacity), samples(alloc), sorted(alloc) {};

    void push(unsigned data);
    void clear() { samples.clear(), mean = m2 = .0f; }

    float variance() const { return mean * m2 / samples.size(); }
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
bool ImHDRColorEdit(const char* label, float3& color, float& power, float maxScale = 100.0f);