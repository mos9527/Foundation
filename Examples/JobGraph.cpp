// Standalone self-test for Foundation::Core::JobGraph. Links only Foundation_Core; returns non-zero
// if any expectation fails. Exercises the cases called out in the plan: empty / barrier-only graphs,
// worker->worker, worker->main, main->worker dependencies, parallel-for fanout, ExecutionPolicy::Seq,
// a worker-less pool, multi-producer joins, and the Wait() main-thread pump.
#include <Core/JobGraph.hpp>
#include <atomic>
#include <cstdio>
#include <thread>

using namespace Foundation::Core;

static int gFailures = 0;
#define EXPECT(cond, msg)                                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::printf("FAIL: %s\n", msg);                                                                            \
            ++gFailures;                                                                                               \
        }                                                                                                              \
    } while (0)

static size_t MakeTaskSize(size_t want) { return ThreadPool::CalcTaskSize(want < 64 ? 64 : want); }

// Empty graph and barrier-only graph must submit and drain without work.
static void TestEmptyAndBarrier(ThreadPool& pool)
{
    {
        JobGraph g(pool, GLOBAL_ALLOC);
        g.Submit();
        g.Wait();
    }
    {
        JobGraph g(pool, GLOBAL_ALLOC);
        JobHandle a = g.AddBarrier("a");
        JobHandle b = g.AddBarrier("b");
        JobHandle c = g.AddBarrier("c");
        g.DependsOn(b, a);
        g.DependsOn(c, b);
        g.Submit();
        g.Wait(c);
    }
}

// Parallel-for fans out and visits every index exactly once.
static void TestParallelForSum(ThreadPool& pool)
{
    constexpr size_t N = 10000;
    std::atomic<size_t> sum{0};
    std::atomic<size_t> visits{0};
    JobGraph g(pool, GLOBAL_ALLOC);
    JobHandle h = g.AddParallelFor("sum", ExecutionPolicy::Par, N, [&](size_t i)
                                   {
                                       sum.fetch_add(i, std::memory_order_relaxed);
                                       visits.fetch_add(1, std::memory_order_relaxed);
                                   });
    g.Submit();
    g.Wait(h);
    EXPECT(visits.load() == N, "parallel-for visited every index once");
    EXPECT(sum.load() == N * (N - 1) / 2, "parallel-for sum");
}

// pose(parallel-for) -> begin(main) -> deform(parallel-for) -> end(main) -> done(barrier).
// Verifies main nodes run on the waiting thread and the strict ordering holds.
static void TestChainOrdering(ThreadPool& pool)
{
    constexpr size_t N = 256;
    auto const mainId = std::this_thread::get_id();
    std::atomic<bool> beginDone{false};
    std::atomic<bool> deformSawBegin{true};
    std::atomic<size_t> deformCount{0};
    std::atomic<bool> endDone{false};

    JobGraph g(pool, GLOBAL_ALLOC);
    JobHandle done = g.AddBarrier("done");
    JobHandle pose = g.AddParallelFor("pose", ExecutionPolicy::Par, 16, [](size_t) {});
    JobHandle begin = g.AddMain("begin", [&]
                                {
                                    EXPECT(std::this_thread::get_id() == mainId, "begin runs on main thread");
                                    beginDone.store(true, std::memory_order_release);
                                });
    JobHandle deform = g.AddParallelFor("deform", ExecutionPolicy::Par, N, [&](size_t)
                                        {
                                            if (!beginDone.load(std::memory_order_acquire))
                                                deformSawBegin.store(false, std::memory_order_relaxed);
                                            deformCount.fetch_add(1, std::memory_order_relaxed);
                                        });
    JobHandle end = g.AddMain("end", [&]
                              {
                                  EXPECT(std::this_thread::get_id() == mainId, "end runs on main thread");
                                  EXPECT(deformCount.load(std::memory_order_relaxed) == N, "deform completes before end");
                                  endDone.store(true, std::memory_order_relaxed);
                              });
    g.DependsOn(begin, pose);
    g.DependsOn(deform, begin);
    g.DependsOn(end, deform);
    g.DependsOn(done, end);
    g.Submit();
    g.Wait(done);
    EXPECT(deformSawBegin.load(std::memory_order_relaxed), "deform runs strictly after begin");
    EXPECT(endDone.load(std::memory_order_relaxed), "end ran");
}

// Diamond: A(parallel-for) -> B(worker), A -> C(main), {B, C} -> D(barrier). Also covers a plain
// worker->worker edge and a multi-producer join.
static void TestDiamond(ThreadPool& pool)
{
    auto const mainId = std::this_thread::get_id();
    std::atomic<bool> aDone{false};
    std::atomic<bool> bSawA{true}, cSawA{true};
    std::atomic<bool> bDone{false}, cDone{false};
    std::atomic<bool> dSawBoth{true};

    JobGraph g(pool, GLOBAL_ALLOC);
    JobHandle d = g.AddBarrier("D");
    JobHandle a = g.AddParallelFor("A", ExecutionPolicy::Par, 32, [&](size_t)
                                   { aDone.store(true, std::memory_order_release); });
    JobHandle b = g.AddJob("B", ExecutionPolicy::Par, [&]
                           {
                               if (!aDone.load(std::memory_order_acquire))
                                   bSawA.store(false, std::memory_order_relaxed);
                               bDone.store(true, std::memory_order_release);
                           });
    JobHandle c = g.AddMain("C", [&]
                            {
                                EXPECT(std::this_thread::get_id() == mainId, "C runs on main thread");
                                if (!aDone.load(std::memory_order_acquire))
                                    cSawA.store(false, std::memory_order_relaxed);
                                cDone.store(true, std::memory_order_release);
                            });
    JobHandle e = g.AddMain("checkD", [&]
                            {
                                if (!bDone.load(std::memory_order_acquire) || !cDone.load(std::memory_order_acquire))
                                    dSawBoth.store(false, std::memory_order_relaxed);
                            });
    g.DependsOn(b, a);
    g.DependsOn(c, a);
    g.DependsOn(e, b, c);
    g.DependsOn(d, e);
    g.Submit();
    g.Wait(d);
    EXPECT(bSawA.load(std::memory_order_relaxed), "worker B runs after A");
    EXPECT(cSawA.load(std::memory_order_relaxed), "main C runs after A");
    EXPECT(dSawBoth.load(std::memory_order_relaxed), "join runs after both producers");
}

// ExecutionPolicy::Seq parallel-for runs serially on the waiting (main) thread.
static void TestSeqOnMain(ThreadPool& pool)
{
    constexpr size_t N = 1000;
    auto const mainId = std::this_thread::get_id();
    std::atomic<size_t> sum{0};
    std::atomic<bool> onMain{true};
    JobGraph g(pool, GLOBAL_ALLOC);
    JobHandle h = g.AddParallelFor("seq", ExecutionPolicy::Seq, N, [&](size_t i, size_t worker)
                                   {
                                       if (std::this_thread::get_id() != mainId || worker != g.MainWorkerId())
                                           onMain.store(false, std::memory_order_relaxed);
                                       sum.fetch_add(i, std::memory_order_relaxed);
                                   });
    g.Submit();
    g.Wait(h);
    EXPECT(onMain.load(std::memory_order_relaxed), "Seq parallel-for runs on main with MainWorkerId");
    EXPECT(sum.load() == N * (N - 1) / 2, "Seq parallel-for sum");
}

// A worker-less pool must still complete: every node is routed to the Wait/Pump thread.
static void TestNoWorkers()
{
    ThreadPool pool(0, MakeTaskSize(64), GLOBAL_ALLOC, "JG0");
    constexpr size_t N = 500;
    std::atomic<size_t> sum{0};
    std::atomic<bool> mainRan{false};
    JobGraph g(pool, GLOBAL_ALLOC);
    JobHandle pf = g.AddParallelFor("pf", ExecutionPolicy::Par, N, [&](size_t i)
                                    { sum.fetch_add(i, std::memory_order_relaxed); });
    JobHandle m = g.AddMain("m", [&] { mainRan.store(true, std::memory_order_relaxed); });
    JobHandle done = g.AddBarrier("done");
    g.DependsOn(m, pf);
    g.DependsOn(done, m);
    g.Submit();
    g.Wait(done);
    EXPECT(sum.load() == N * (N - 1) / 2, "worker-less parallel-for sum");
    EXPECT(mainRan.load(std::memory_order_relaxed), "worker-less main node ran");
}

// Hammer the chain repeatedly to shake out main/worker hand-off races.
static void TestStress(ThreadPool& pool)
{
    for (int iter = 0; iter < 500; ++iter)
        TestChainOrdering(pool);
}

int main()
{
    {
        ThreadPool pool(4, MakeTaskSize(256), GLOBAL_ALLOC, "JG4");
        TestEmptyAndBarrier(pool);
        TestParallelForSum(pool);
        TestChainOrdering(pool);
        TestDiamond(pool);
        TestSeqOnMain(pool);
        TestStress(pool);
        pool.Join();
    }
    TestNoWorkers();

    if (gFailures == 0)
        std::printf("JobGraph self-test: ALL PASS\n");
    else
        std::printf("JobGraph self-test: %d FAILURE(S)\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
