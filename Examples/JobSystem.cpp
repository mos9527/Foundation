#include <Core/JobSystem.hpp>
#include <array>
#include <iostream>
#include <numeric>

using namespace Foundation::Core;

namespace
{
    bool Require(bool condition, char const* message)
    {
        if (!condition)
            std::cerr << "JobSystem smoke test failed: " << message << '\n';
        return condition;
    }
}

int main()
{
    size_t const hardwareThreads = std::max<size_t>(std::thread::hardware_concurrency(), 2);
    JobSystem jobs({
        .workerCount = hardwareThreads - 1,
        .maxJobs = 128,
        .maxBarriers = 8,
        .readyQueueSize = 128,
        .allocator = GLOBAL_ALLOC,
        .name = "ExampleJob",
    });

    Atomic<int> parsed{0};
    Atomic<int> fanInResult{0};
    JobBarrier graphBarrier = jobs.CreateBarrier();
    JobHandle parse = jobs.CreateJob("Parse", JobPriority::High, [&] { parsed.store(1, std::memory_order_release); });
    JobHandle texture = jobs.CreateJobAfter(
        "Texture", JobPriority::Normal, Span<const JobHandle>{&parse, 1},
        [&]
        {
            CHECK(parsed.load(std::memory_order_acquire) == 1);
            fanInResult.fetch_add(2, std::memory_order_relaxed);
        });
    JobHandle geometry = jobs.CreateJobAfter(
        "Geometry", JobPriority::Normal, Span<const JobHandle>{&parse, 1},
        [&]
        {
            CHECK(parsed.load(std::memory_order_acquire) == 1);
            fanInResult.fetch_add(3, std::memory_order_relaxed);
        });
    Array<JobHandle, 2> cooked{texture, geometry};
    JobHandle commit = jobs.CreateJobAfter(
        "Commit", JobPriority::High, cooked,
        [&]
        {
            fanInResult.fetch_add(1, std::memory_order_relaxed);
        });
    Array<JobHandle, 4> graph{parse, texture, geometry, commit};
    graphBarrier.Add(graph);
    jobs.Wait(graphBarrier);
    if (!Require(fanInResult.load(std::memory_order_relaxed) == 6, "fan-out/fan-in graph"))
        return 1;

    constexpr int kDynamicJobs = 16;
    Atomic<int> dynamicSum{0};
    JobBarrier dynamicBarrier = jobs.CreateBarrier();
    JobHandle finish = jobs.CreateJob("Finish dynamic work", JobPriority::High, [] {}, 1);
    dynamicBarrier.Add(finish);
    JobDependency finishGate = finish.AdoptDependencyGuard();
    JobHandle discover = jobs.CreateJob(
        "Discover dynamic work", JobPriority::Normal,
        [&, finishGate = std::move(finishGate)](JobContext&) mutable
        {
            JobDependency releaseFinish = std::move(finishGate);
            for (int i = 0; i < kDynamicJobs; ++i)
            {
                JobDependency childDone = finish.AddDependencyGuard();
                JobHandle child = jobs.CreateJob(
                    "Dynamic child", JobPriority::Normal,
                    [&, i, childDone = std::move(childDone)]() mutable
                    {
                        JobDependency releaseChild = std::move(childDone);
                        dynamicSum.fetch_add(i, std::memory_order_relaxed);
                    });
                dynamicBarrier.Add(child);
            }
        });
    dynamicBarrier.Add(discover);
    jobs.Wait(dynamicBarrier);
    if (!Require(dynamicSum.load(std::memory_order_relaxed) == (kDynamicJobs - 1) * kDynamicJobs / 2,
                 "dynamic job discovery"))
        return 1;

    std::array<int, 257> values{};
    jobs.Wait(jobs.ParallelFor(
        ExecutionPolicy::Par, "Fill values", values.size(), 13,
        [&](size_t begin, size_t end, JobContext&)
        {
            for (size_t i = begin; i < end; ++i)
                values[i] = static_cast<int>(i);
        }));
    int const expected = static_cast<int>((values.size() - 1) * values.size() / 2);
    if (!Require(std::accumulate(values.begin(), values.end(), 0) == expected, "parallel-for"))
        return 1;

    JobBarrier failureBarrier = jobs.CreateBarrier();
    JobHandle failure = jobs.CreateJob("Expected failure", [] { throw std::runtime_error("expected"); });
    failureBarrier.Add(failure);
    bool caught = false;
    try
    {
        jobs.Wait(failureBarrier);
    }
    catch (std::runtime_error const&)
    {
        caught = true;
    }
    if (!Require(caught, "exception propagation"))
        return 1;

    jobs.Drain();
    std::cout << "JobSystem smoke test passed\n";
    return 0;
}
