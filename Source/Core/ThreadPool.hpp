#pragma once
#include "AtomicQueue.hpp"
#include "Logging.hpp"
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
    struct Job
    {
        virtual ~Job() = default;
        virtual void Execute(size_t id) noexcept = 0;
    };
    template <typename Lambda, typename ReturnType, typename... Args>
    struct LambdaJob final : public Job
    {
        Lambda mFunc;
        Promise<ReturnType> mPromise;
        LambdaJob(Lambda&& func) : mFunc(std::forward<Lambda>(func)) {}
        void Execute(size_t) noexcept override
        {
            if constexpr (std::is_same_v<ReturnType, void>)
            {
                mFunc();
                mPromise.set_value();
            }
            else
                mPromise.set_value(mFunc());
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
            bool const submitting = BeginSubmit();
            CHECK_MSG(submitting, "ThreadPool shutting down");
            if (!submitting)
                return;
            CHECK_MSG(!mThreads.empty(), "ThreadPool has no worker threads");
            CHECK_MSG(PriorityIndex(priority) < kJobPriorityCount, "Invalid job priority");
            if (mThreads.empty() || PriorityIndex(priority) >= kJobPriorityCount)
            {
                EndSubmit();
                return;
            }
            Allocator* allocator = jobAllocator ? jobAllocator : mAllocator;
            auto task = ConstructUniqueBase<Job, T>(allocator, std::forward<Args>(args)...);
            mTotal.fetch_add(1, std::memory_order_release);
            bool const queued = mJobs[PriorityIndex(priority)].Push(std::move(task));
            CHECK_MSG(queued, "ThreadPool job queue full");
            if (!queued)
            {
                mTotal.fetch_sub(1, std::memory_order_release);
                EndSubmit();
                return;
            }
            mWakeEpoch.fetch_add(1, std::memory_order_release);
            mWakeEpoch.notify_one();
            EndSubmit();
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
        ThreadPool(size_t numThreads, size_t maxTasks, Allocator* alloc, StringView name = "ThreadPool");
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
        /** @brief Join accepted jobs and stop all workers. */
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
