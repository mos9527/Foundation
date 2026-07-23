#include "JoltCommon.hpp"
#include <Jolt/RegisterTypes.h>
#include <array>
#include <iostream>

using Foundation::Core::Atomic;
using Foundation::Examples::FoundationJoltJobSystem;

int main()
{
    JPH::RegisterDefaultAllocator();

    constexpr int kJobCount = 128;
    Foundation::Core::JobSystem scheduler({
        .workerCount = 4,
        .maxJobs = kJobCount,
        .maxBarriers = 4,
        .readyQueueSize = kJobCount,
        .allocator = GLOBAL_ALLOC,
        .name = "JoltSmoke",
    });
    FoundationJoltJobSystem jobs(&scheduler, kJobCount, 4);
    JPH::JobSystem::Barrier* barrier = jobs.CreateBarrier();
    if (!barrier)
        return 1;

    Atomic<uint32_t> sequence{1};
    std::array<Atomic<uint32_t>, kJobCount> values{};
    std::array<JPH::JobHandle, kJobCount> handles;
    for (int i = 0; i < kJobCount; ++i)
    {
        handles[i] = jobs.CreateJob(
            "Foundation Jolt chain", JPH::Color::sGreen,
            [&, i]
            {
                values[i].store(sequence.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
                if (i > 0)
                    handles[i - 1].RemoveDependency();
            },
            1);
        barrier->AddJob(handles[i]);
    }

    handles.back().RemoveDependency();
    jobs.WaitForJobs(barrier);
    jobs.DestroyBarrier(barrier);

    for (int i = kJobCount - 1; i >= 0; --i)
    {
        uint32_t const expected = static_cast<uint32_t>(kJobCount - i);
        if (values[i].load(std::memory_order_relaxed) != expected)
        {
            std::cerr << "Foundation Jolt job-system chain failed at " << i << '\n';
            return 1;
        }
    }

    std::cout << "Foundation Jolt job-system smoke test passed\n";
    return 0;
}
