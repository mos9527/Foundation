#pragma once
#include "Logging.hpp"
#include "ThreadPool.hpp"
#include <condition_variable>
#include <mutex>
namespace Foundation::Core
{
    /**
     * @brief Opaque handle to a node in a @ref JobGraph. Returned by the @c Add* builders and passed
     *        to @ref JobGraph::DependsOn / @ref JobGraph::Wait.
     */
    struct JobHandle
    {
        static constexpr size_t kInvalid = ~static_cast<size_t>(0);
        size_t id{kInvalid};
        [[nodiscard]] bool Valid() const noexcept { return id != kInvalid; }
    };

    /**
     * @brief A small, transient CPU job graph layered on top of @ref ThreadPool.
     *
     * @details The pool stays a flat worker executor; @ref JobGraph adds first-class dependency
     *          handles so a caller can express a frame's CPU work as nodes + edges instead of
     *          hand-placed futures. A node becomes schedulable once all of its producers complete:
     *          - @c Worker / @c ParallelFor nodes (with @ref ExecutionPolicy::Par and a pool that has
     *            workers) dispatch to the pool. A @c ParallelFor fans out across the workers using a
     *            shared cursor + a fork-join latch, exactly like @ref ThreadPool::ParallelForAsync.
     *          - @c Main nodes (and any node forced serial via @ref ExecutionPolicy::Seq, or when the
     *            pool has no workers) run on the thread that calls @ref Wait / @ref PumpMainThread,
     *            never on a worker. This is the home for main-thread-only steps.
     *          - @c Barrier nodes carry no work; they complete as soon as their producers do, useful
     *            as a single join handle to @ref Wait on.
     *
     *          Build (single-threaded) -> @ref Submit -> do other work -> @ref Wait. The graph object
     *          must outlive its in-flight work; @ref Wait (and the destructor) block until it drains.
     *
     * @note Worker ids handed to node bodies follow the pool's contract: pool workers see ids in
     *       [0, @ref ThreadPool::GetWorkerCount); main-thread nodes see @ref MainWorkerId() (==
     *       worker count), matching @ref ThreadPool::GetParallelForConcurrency so per-worker scratch
     *       sized to that concurrency stays race-free.
     * @note Scope is intentionally small: no cancellation, no result values, no persistent/nested
     *       graphs. Build a fresh graph per logical batch.
     */
    class JobGraph
    {
    public:
        enum class NodeKind
        {
            Worker, // single body, runs once on a pool worker (or main if no workers)
            ParallelFor, // body invoked per index, fanned out across the pool
            Main, // single body, runs on the Wait/Pump thread
            Barrier, // no body; completes when its producers complete
        };

    private:
        // Type-erased node body. Single-shot nodes use RunSingle; parallel-for nodes use RunIndex.
        struct IJobWork
        {
            virtual ~IJobWork() = default;
            virtual void RunSingle(size_t /*workerId*/) {}
            virtual void RunIndex(size_t /*index*/, size_t /*workerId*/) {}
        };
        template <typename Fn>
        struct SingleWork final : IJobWork
        {
            Fn fn;
            explicit SingleWork(Fn&& f) : fn(std::move(f)) {}
            void RunSingle(size_t workerId) override
            {
                if constexpr (std::is_invocable_v<Fn&, size_t>)
                    fn(workerId);
                else
                    fn();
            }
        };
        template <typename Fn>
        struct IndexWork final : IJobWork
        {
            Fn fn;
            explicit IndexWork(Fn&& f) : fn(std::move(f)) {}
            void RunIndex(size_t index, size_t workerId) override { ParallelForInvoke(fn, index, workerId); }
        };

        struct Node
        {
            NodeKind kind{NodeKind::Barrier};
            ExecutionPolicy policy{ExecutionPolicy::Par};
            UniquePtr<IJobWork> work{nullptr, StlDeleter<IJobWork>{nullptr}};
            size_t total{0}; // parallel-for element count
            StringView name;
            // Scheduling state guarded by JobGraph::mMutex.
            size_t pending{0}; // unmet producer dependencies
            bool finished{false};
            Vector<size_t> dependents;
            // Parallel-for hot state (touched lock-free by the fanned-out workers).
            Atomic<size_t> cursor{0};
            Atomic<size_t> remaining{0};
            explicit Node(Allocator* alloc) : dependents(alloc) {}
        };

        // Pool jobs that drive node bodies; defined in the .cpp (need the full JobGraph).
        struct WorkerJob;
        struct ParallelForJob;

        ThreadPool& mPool;
        Allocator* mAllocator;
        Vector<UniquePtr<Node>> mNodes;
        Mutex mMutex;
        CondVar mCond;
        Queue<size_t> mMainReady; // node ids ready to run on the Wait/Pump thread
        size_t mNodesRemaining{0};
        size_t mMainWorkerId{0};
        bool mSubmitted{false};

        size_t AddNode(NodeKind kind, StringView name, UniquePtr<IJobWork> work, size_t total,
                       ExecutionPolicy policy);
        void AddEdge(size_t producer, size_t consumer);
        void Schedule(size_t id);
        void DispatchWorker(size_t id);
        void DispatchParallelFor(size_t id);
        void EnqueueMain(size_t id);
        void RunMainNode(size_t id);
        void OnNodeFinished(size_t id);

    public:
        JobGraph(ThreadPool& pool, Allocator* allocator);
        JobGraph(JobGraph const&) = delete;
        JobGraph& operator=(JobGraph const&) = delete;
        ~JobGraph();

        /** @brief A single body that runs once on a pool worker (or the main thread under Seq / no workers). */
        template <typename Fn>
        JobHandle AddJob(StringView name, ExecutionPolicy policy, Fn&& fn)
        {
            auto work = ConstructUniqueBase<IJobWork, SingleWork<std::remove_reference_t<Fn>>>(
                mAllocator, std::forward<Fn>(fn));
            return {AddNode(NodeKind::Worker, name, std::move(work), 0, policy)};
        }
        /** @brief A single body that always runs on the thread calling @ref Wait / @ref PumpMainThread. */
        template <typename Fn>
        JobHandle AddMain(StringView name, Fn&& fn)
        {
            auto work = ConstructUniqueBase<IJobWork, SingleWork<std::remove_reference_t<Fn>>>(
                mAllocator, std::forward<Fn>(fn));
            return {AddNode(NodeKind::Main, name, std::move(work), 0, ExecutionPolicy::Seq)};
        }
        /** @brief Index parallel-for body invoked as @c fn(i) or @c fn(i, workerId) for each index. */
        template <typename Fn>
        JobHandle AddParallelFor(StringView name, ExecutionPolicy policy, size_t count, Fn&& fn)
        {
            auto work = ConstructUniqueBase<IJobWork, IndexWork<std::remove_reference_t<Fn>>>(
                mAllocator, std::forward<Fn>(fn));
            return {AddNode(NodeKind::ParallelFor, name, std::move(work), count, policy)};
        }
        /** @brief Iterator-range parallel-for (random-access iterators), like the @ref ThreadPool overload. */
        template <typename It, typename Fn>
            requires std::random_access_iterator<It>
        JobHandle AddParallelFor(StringView name, ExecutionPolicy policy, It first, It last, Fn&& fn)
        {
            auto const count = last - first;
            size_t const total = count <= 0 ? 0 : static_cast<size_t>(count);
            return AddParallelFor(name, policy, total,
                [first, fn = std::forward<Fn>(fn)](size_t i, size_t worker) mutable
                { ParallelForInvoke(fn, first[static_cast<std::iter_difference_t<It>>(i)], worker); });
        }
        /** @brief A work-less join node that completes once all its producers complete. */
        JobHandle AddBarrier(StringView name);

        /**
         * @brief Declares that @p consumer may only run after every @p producers has completed.
         * @note Build-time only (before @ref Submit). Edges are not deduplicated.
         */
        template <typename... Producers>
        void DependsOn(JobHandle consumer, Producers... producers)
        {
            (AddEdge(producers.id, consumer.id), ...);
        }

        /** @brief Arms the graph and schedules all dependency-free nodes. Call exactly once. */
        void Submit();
        /** @brief Runs ready main-thread nodes on the caller, then blocks until @p target completes. */
        void Wait(JobHandle target);
        /** @brief Blocks until the whole graph completes (pumping main-thread nodes on the caller). */
        void Wait() { Wait(JobHandle{}); }
        /** @brief Runs every currently-ready main-thread node on the caller without blocking. */
        void PumpMainThread();

        /** @brief Worker id handed to main-thread node bodies (== @ref ThreadPool::GetWorkerCount). */
        [[nodiscard]] size_t MainWorkerId() const noexcept { return mMainWorkerId; }
    };
} // namespace Foundation::Core
