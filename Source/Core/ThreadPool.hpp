#pragma once
#include "AtomicQueue.hpp"
#include "Thread.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <type_traits>
namespace Foundation::Core
{
    enum class JobPriority : size_t
    {
        Low,
        Normal,
        High,
    };
    inline constexpr size_t kJobPriorityCount = static_cast<size_t>(JobPriority::High) + 1;

    /**
     * @brief Runtime execution policy for @ref ThreadPool::ParallelFor, mirroring std::execution's
     *        seq/par — but chosen at runtime, so a caller can downgrade to serial with a one-argument
     *        change (debugging, determinism, or tiny workloads) instead of a compile-time switch.
     */
    enum class ExecutionPolicy
    {
        Seq, // run inline on the calling thread, in index order
        Par, // fork-join across the pool (default)
    };

    /**
     * @brief Job interface for use with @ref ThreadPool
     *
     * Custom implementations of @ref ThreadPoolJob can be constructed in-place with @ref ThreadPool::PushImpl
     * For simple, stateless jobs, consider using @ref ThreadPool::Push with a lambda instead.
     */
    struct Job
    {
        virtual ~Job() = default;
        virtual void Execute(size_t id) noexcept = 0;
    };
    /**
     * @brief State-carrying lambda job for use with @ref ThreadPool
     *
     * @note This is not meant to be used directly. Instead, use @ref ThreadPool::Push with a lambda function.
     *
     * @tparam Lambda Type of the lambda function.
     * @tparam ReturnType Return type of the lambda function.
     */
    template <typename Lambda, typename ReturnType, typename... Args>
    struct LambdaJob final : public Job
    {
        Lambda mFunc;
        Promise<ReturnType> mPromise;
        LambdaJob(Lambda&& func) : mFunc(std::forward<Lambda>(func)) {}
        void Execute(size_t) noexcept override
        {
            try
            {
                if constexpr (std::is_same_v<ReturnType, void>)
                {
                    mFunc();
                    mPromise.set_value();
                }
                else
                    mPromise.set_value(mFunc());
            }
            catch (...)
            {
                mPromise.set_exception(std::current_exception());
            }
        }
    };
    /**
     * @brief Invokes a @ref ThreadPool::ParallelFor body for one item, passing the worker id only if
     *        the functor accepts it. @p arg is the index (index form) or the dereferenced element
     *        (iterator form), so callers may write `fn(x)` or `fn(x, workerId)` as they like.
     */
    template <typename Fn, typename Arg>
    inline void ParallelForInvoke(Fn& fn, Arg&& arg, size_t workerId)
    {
        if constexpr (std::is_invocable_v<Fn&, Arg&&, size_t>)
            fn(std::forward<Arg>(arg), workerId);
        else
            fn(std::forward<Arg>(arg));
    }

    /**
     * @brief Self-draining for-loop job used by @ref ThreadPool::ParallelFor.
     * @details One instance is co-invoked across N workers (+ the calling thread) via
     *          @ref ThreadPool::CoInvoke; each pulls indices from a shared atomic cursor until the
     *          range is exhausted, invoking @p fn per index. It lives on the caller's stack — no heap
     *          allocation, no future. @p fn is invoked concurrently as `fn(i)` / `fn(i, workerId)`,
     *          so it must be thread-safe: shared captures read-only, writes disjoint per index, any
     *          scratch keyed by @p workerId (a worker never runs two invocations at once, so
     *          per-worker scratch is race-free). Per-index granularity is intentional — a job that
     *          wants coarser work batches itself by choosing @p count.
     */
    template <typename Fn>
    struct ParallelForJob final : Job
    {
        Fn* fn{nullptr};
        size_t total{0};
        Atomic<size_t> cursor{0};
        Atomic<size_t> remaining{0}; // co-invocations still running (fork-join latch)
        void Execute(size_t workerId) noexcept override
        {
            for (size_t i; (i = cursor.fetch_add(1, std::memory_order_relaxed)) < total;)
                ParallelForInvoke(*fn, i, workerId);
            if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                remaining.notify_all();
        }
    };

    /**
     * @brief Backing job queue type for @ref ThreadPool
     */
    using JobQueue = MPMCQueue<UniquePtr<Job>>;
    using JobQueues = std::array<JobQueue, kJobPriorityCount>;
    /**
     * @brief Atomic, lock-free Thread Pool implementation with fixed bounds
     */
    class ThreadPool
    {
        Allocator* mAllocator;
        String mName;
        Atomic<bool> mShutdown{false};
        Atomic<size_t> mComplete{0};
        Atomic<size_t> mTotal{0};

        JobQueues mJobs;
        // Ensure threads are joined first on destruction
        Vector<Thread> mThreads;
        void ThreadPoolWorker(size_t id);
        static constexpr size_t PriorityIndex(JobPriority priority) noexcept { return static_cast<size_t>(priority); }
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        T* PushImplInternal(JobPriority priority, Allocator* jobAllocator, Args&&... args)
        {
            if (mShutdown)
                throw std::runtime_error("ThreadPool shutting down");
            if (PriorityIndex(priority) >= kJobPriorityCount)
                throw std::runtime_error("Invalid job priority");
            Allocator* allocator = jobAllocator ? jobAllocator : mAllocator;
            auto task = ConstructUniqueBase<Job, T>(allocator, std::forward<Args>(args)...);
            T* ptr = static_cast<T*>(task.get());
            if (!mJobs[PriorityIndex(priority)].Push(std::move(task)))
                throw std::runtime_error("Jobs full");
            mTotal.fetch_add(1, std::memory_order_relaxed);
            mTotal.notify_one();
            return ptr;
        }
        template <typename Lambda, typename... Args>
        auto PushLambdaInternal(JobPriority priority, Allocator* jobAllocator, Lambda&& func, Args const&... args)
        {
            auto LambdaFn = [func = std::forward<Lambda>(func), ... args = args] { return func(args...); };
            using LambdaType = decltype(LambdaFn);
            using ReturnType = decltype(LambdaFn());
            LambdaJob<LambdaType, ReturnType> job(std::forward<LambdaType>(LambdaFn));
            auto fut = job.mPromise.get_future();
            PushImplInternal<LambdaJob<LambdaType, ReturnType>>(priority, jobAllocator, std::move(job));
            return std::move(fut);
        }

    public:
        /**
         * @brief Construct a thread pool with the given number of worker threads.
         * @param numThreads Number of worker threads to spawn.
         * @param maxTasks Max number of tasks that can be queued per priority. Must be a power of two - see @ref getTaskSize
         * @param alloc Allocator to use for internal and job allocations
         * @param name Prefix for worker thread names ("name@id")
         */
        ThreadPool(size_t numThreads, size_t maxTasks, Allocator* alloc, StringView name = "ThreadPool");
        /**
         * @brief Push a job implementing @ref ThreadPoolJob to the thread pool.
         * @note This by itself does not return a future or any way to get the result of the job
         *       It's up to the implementation of the job to provide a way to get the result.
         *       See also @ref ThreadPoolLambdaJob
         * @return Stable pointer of the pushed job. Lifetime guaranteed until the job's completion.
         */
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        T* PushImpl(JobPriority priority, Args&&... args)
        {
            return PushImplInternal<T>(priority, nullptr, std::forward<Args>(args)...);
        }
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        T* PushImpl(Args&&... args)
        {
            return PushImplInternal<T>(JobPriority::Normal, nullptr, std::forward<Args>(args)...);
        }
        /**
         * @brief Push a job with an explicit allocator for the job object.
         * @param jobAllocator Optional allocator for the job object. If null, the thread pool allocator is used.
         * @return Stable pointer of the pushed job. Lifetime guaranteed until the job's completion.
         */
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        T* PushImplAlloc(Allocator* jobAllocator, Args&&... args)
        {
            return PushImplInternal<T>(JobPriority::Normal, jobAllocator, std::forward<Args>(args)...);
        }
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        T* PushImplAlloc(JobPriority priority, Allocator* jobAllocator, Args&&... args)
        {
            return PushImplInternal<T>(priority, jobAllocator, std::forward<Args>(args)...);
        }
        /**
         * @brief Push a lambda job to the thread pool.
         * @return @ref Future<func ReturnType> that will be set when the job is completed.
         */
        template <typename Lambda, typename... Args>
        auto Push(JobPriority priority, Lambda&& func, Args const&... args)
        {
            return PushLambdaInternal(priority, nullptr, std::forward<Lambda>(func), args...);
        }
        template <typename Lambda, typename... Args>
        auto Push(Lambda&& func, Args const&... args)
        {
            return PushLambdaInternal(JobPriority::Normal, nullptr, std::forward<Lambda>(func), args...);
        }
        /**
         * @brief Push a lambda job with an explicit allocator for the job object.
         * @param jobAllocator Optional allocator for the job object. If null, the thread pool allocator is used.
         */
        template <typename Lambda, typename... Args>
        auto PushAlloc(Allocator* jobAllocator, Lambda&& func, Args const&... args)
        {
            return PushLambdaInternal(JobPriority::Normal, jobAllocator, std::forward<Lambda>(func), args...);
        }
        template <typename Lambda, typename... Args>
        auto PushAlloc(JobPriority priority, Allocator* jobAllocator, Lambda&& func, Args const&... args)
        {
            return PushLambdaInternal(priority, jobAllocator, std::forward<Lambda>(func), args...);
        }
        /**
         * @brief Enqueues @p count *non-owning* references to a single, externally-owned job so that
         *        up to @p count workers co-run it concurrently (each call to @ref ThreadPoolJob::Execute
         *        receives that worker's id). The pool never destroys or frees @p job — the caller owns
         *        its lifetime and MUST keep it alive until all co-invocations complete (e.g. via a
         *        fork-join latch). Used by @ref ParallelFor for a self-draining, zero-allocation job.
         * @note @p count is typically <= @ref GetWorkerCount; worker ids passed to Execute are in
         *       [0, GetWorkerCount). A participating caller should use id == GetWorkerCount.
         */
        void CoInvoke(Job& job, size_t count, JobPriority priority = JobPriority::Normal);

        /** @brief Number of worker threads. Worker ids passed to Execute are in [0, this). */
        [[nodiscard]] size_t GetWorkerCount() const noexcept { return mThreads.size(); }

        /**
         * @brief Number of distinct worker ids a @ref ParallelFor functor may see (workers + the
         *        participating caller). Size per-worker scratch to this.
         */
        [[nodiscard]] size_t GetParallelForConcurrency() const noexcept { return mThreads.size() + 1; }

        /**
         * @brief Parallel-for over [0, @p count): invokes @p fn for every index as `fn(i)` or
         *        `fn(i, workerId)` (the worker id is passed only if the functor accepts it), blocking
         *        until all indices are processed.
         * @details Main-thread-initiated and non-nesting. With @ref ExecutionPolicy::Par it reuses the
         *          pool's workers and the calling thread (which participates with id == @ref
         *          GetWorkerCount), pushing only non-owning references to a stack-resident
         *          @ref ParallelForJob — zero per-call allocation, no futures. @ref ExecutionPolicy::Seq
         *          (and a worker-less pool) runs inline in order. @p fn must be safe to invoke
         *          concurrently under Par; key any scratch by @p workerId. Granularity is one index per
         *          call: a job that wants coarser work batches itself by choosing @p count.
         */
        template <typename Fn>
        void ParallelFor(ExecutionPolicy policy, size_t count, Fn&& fn)
        {
            if (count == 0)
                return;
            size_t const workers = mThreads.size();
            if (policy == ExecutionPolicy::Seq || workers == 0)
            {
                for (size_t i = 0; i < count; ++i)
                    ParallelForInvoke(fn, i, size_t{0}); // inline: caller is worker 0
                return;
            }
            ParallelForJob<std::remove_reference_t<Fn>> job;
            job.fn = &fn;
            job.total = count;
            size_t const helpers = std::min(workers, count);
            job.remaining.store(helpers + 1, std::memory_order_relaxed); // + the participating caller
            CoInvoke(job, helpers);
            // Participate on the calling thread with id == workers (its own scratch slot).
            for (size_t i; (i = job.cursor.fetch_add(1, std::memory_order_relaxed)) < count;)
                ParallelForInvoke(fn, i, workers);
            // Fork-join: every co-invocation must return before the stack job dies.
            if (job.remaining.fetch_sub(1, std::memory_order_acq_rel) != 1)
            {
                size_t r;
                while ((r = job.remaining.load(std::memory_order_acquire)) != 0)
                    job.remaining.wait(r, std::memory_order_acquire);
            }
        }
        /** @brief Index parallel-for defaulting to @ref ExecutionPolicy::Par. */
        template <typename Fn>
        void ParallelFor(size_t count, Fn&& fn)
        {
            ParallelFor(ExecutionPolicy::Par, count, std::forward<Fn>(fn));
        }

        /**
         * @brief Iterator-range parallel-for, like a (policy-aware) parallel `std::for_each`: invokes
         *        @p fn for each element in [@p first, @p last) as `fn(elem)` or `fn(elem, workerId)`.
         * @details A thin wrapper over the index form (random-access iterators only): the element is
         *          passed by reference, so @p fn may mutate it (parallel transform). Same concurrency
         *          contract as the index form; @p policy selects serial vs parallel at runtime.
         */
        template <typename It, typename Fn>
            requires std::random_access_iterator<It>
        void ParallelFor(ExecutionPolicy policy, It first, It last, Fn&& fn)
        {
            auto const count = last - first;
            if (count <= 0)
                return;
            ParallelFor(policy, static_cast<size_t>(count), [&](size_t i, size_t worker)
                        { ParallelForInvoke(fn, first[static_cast<std::iter_difference_t<It>>(i)], worker); });
        }
        /** @brief Iterator-range parallel-for defaulting to @ref ExecutionPolicy::Par. */
        template <typename It, typename Fn>
            requires std::random_access_iterator<It>
        void ParallelFor(It first, It last, Fn&& fn)
        {
            ParallelFor(ExecutionPolicy::Par, first, last, std::forward<Fn>(fn));
        }

        /**
         * @brief Shutdown the @ref ThreadPool, potentially cancelling all pending jobs.
         * @note This does not cancel running jobs, but prevents any new jobs from being run/scheduled.
         */
        void Shutdown();
        /**
         * @brief Wait for all scheduled jobs to complete.
         * @note This _MUST_ be called if you'd like all submitted work to complete before destruction.
         */
        void Join();
        /**
         * @brief Shutdown, without waiting for pending jobs.
         */
        ~ThreadPool();

        [[nodiscard]] size_t GetPendingJobCount() const noexcept
        {
            return mTotal.load(std::memory_order_relaxed) - mComplete.load(std::memory_order_relaxed);
        }
        [[nodiscard]] size_t GetCompletedJobCount() const noexcept { return mComplete.load(std::memory_order_relaxed); }
        [[nodiscard]] size_t GetTotalJobCount() const noexcept { return mTotal.load(std::memory_order_relaxed); }

        /**
         * Aligns a number to upper, closest power of 2 so that it's a valid @ref maxTasks size.
         */
        const static size_t CalcTaskSize(size_t size) { return 1ULL << static_cast<size_t>(std::ceil(std::log2f(size))); }
    };
} // namespace Foundation::Core
