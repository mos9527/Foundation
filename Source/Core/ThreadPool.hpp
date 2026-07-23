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
        Atomic<bool> mAccepting{true};
        Mutex mSubmitMutex;
        CondVar mSubmitCV;
        size_t mSubmitting{};
        Atomic<bool> mShutdown{false};
        Atomic<size_t> mWakeEpoch{0};
        Atomic<size_t> mProgressEpoch{0};
        Atomic<size_t> mComplete{0};
        Atomic<size_t> mTotal{0};

        JobQueues mJobs;
        // Ensure threads are joined first on destruction
        Vector<Thread> mThreads;
        void ThreadPoolWorker(size_t id);
        bool BeginSubmit() noexcept
        {
            std::lock_guard lock(mSubmitMutex);
            if (!mAccepting.load(std::memory_order_relaxed))
                return false;
            ++mSubmitting;
            return true;
        }
        void EndSubmit() noexcept
        {
            std::lock_guard lock(mSubmitMutex);
            if (--mSubmitting == 0)
                mSubmitCV.notify_all();
        }
        static constexpr size_t PriorityIndex(JobPriority priority) noexcept { return static_cast<size_t>(priority); }
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        void PushImplInternal(JobPriority priority, Allocator* jobAllocator, Args&&... args)
        {
            if (!BeginSubmit())
                throw std::runtime_error("ThreadPool shutting down");
            try
            {
                if (mThreads.empty())
                    throw std::runtime_error("ThreadPool has no worker threads");
                if (PriorityIndex(priority) >= kJobPriorityCount)
                    throw std::runtime_error("Invalid job priority");
                Allocator* allocator = jobAllocator ? jobAllocator : mAllocator;
                auto task = ConstructUniqueBase<Job, T>(allocator, std::forward<Args>(args)...);
                mTotal.fetch_add(1, std::memory_order_release);
                if (!mJobs[PriorityIndex(priority)].Push(std::move(task)))
                {
                    mTotal.fetch_sub(1, std::memory_order_release);
                    mProgressEpoch.fetch_add(1, std::memory_order_release);
                    mProgressEpoch.notify_all();
                    throw std::runtime_error("Jobs full");
                }
                mWakeEpoch.fetch_add(1, std::memory_order_release);
                mWakeEpoch.notify_one();
                EndSubmit();
                return;
            }
            catch (...)
            {
                EndSubmit();
                throw;
            }
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
         */
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        void PushImpl(JobPriority priority, Args&&... args)
        {
            PushImplInternal<T>(priority, nullptr, std::forward<Args>(args)...);
        }
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        void PushImpl(Args&&... args)
        {
            PushImplInternal<T>(JobPriority::Normal, nullptr, std::forward<Args>(args)...);
        }
        /**
         * @brief Push a job with an explicit allocator for the job object.
         * @param jobAllocator Optional allocator for the job object. If null, the thread pool allocator is used.
         */
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        void PushImplAlloc(Allocator* jobAllocator, Args&&... args)
        {
            PushImplInternal<T>(JobPriority::Normal, jobAllocator, std::forward<Args>(args)...);
        }
        template <typename T, typename... Args>
            requires std::is_base_of_v<Job, T>
        void PushImplAlloc(JobPriority priority, Allocator* jobAllocator, Args&&... args)
        {
            PushImplInternal<T>(priority, jobAllocator, std::forward<Args>(args)...);
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

        /** @brief Number of worker threads. Worker ids passed to Execute are in [0, this). */
        [[nodiscard]] size_t GetWorkerCount() const noexcept { return mThreads.size(); }

        /**
         * @brief Number of distinct worker ids a @ref ParallelFor functor may see (workers + the
         *        participating caller). Size per-worker scratch to this.
         */
        [[nodiscard]] size_t GetParallelForConcurrency() const noexcept { return mThreads.size() + 1; }

        /**
         * @brief Stop accepting work, drain accepted jobs, and stop all workers.
         */
        void Shutdown();
        /**
         * @brief Wait for all scheduled jobs to complete.
         * @note Concurrent submission must be externally synchronized with this call.
         */
        void Join();
        /** @brief Drain accepted jobs and stop all workers. */
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
        const static size_t CalcTaskSize(size_t size) { return std::bit_ceil(std::max<size_t>(size, 1)); }
    };
} // namespace Foundation::Core
