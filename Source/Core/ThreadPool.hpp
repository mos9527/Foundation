#pragma once
#include "AtomicQueue.hpp"
#include "Thread.hpp"
#include <cmath>
namespace Foundation::Core
{
    /**
     * @brief Job interface for use with @ref ThreadPool
     *
     * Custom implementations of @ref ThreadPoolJob can be constructed in-place with @ref ThreadPool::PushImpl
     * For simple, stateless jobs, consider using @ref ThreadPool::Push with a lambda instead.
     */
    struct ThreadPoolJob
    {
        virtual ~ThreadPoolJob() = default;
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
    struct ThreadPoolLambdaJob final : public ThreadPoolJob
    {
        Lambda mFunc;
        Promise<ReturnType> mPromise;
        ThreadPoolLambdaJob(Lambda&& func) : mFunc(std::forward<Lambda>(func)) {}
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
    using JobQueue = MPMCQueue<UniquePtr<ThreadPoolJob>>;
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

        JobQueue mJobs;
        // Ensure threads are joined first on destruction
        Vector<Thread> mThreads;
        void ThreadPoolWorker(size_t id);

    public:
        /**
         * @brief Construct a thread pool with the given number of worker threads.
         * @param numThreads Number of worker threads to spawn.
         * @param maxTasks Max number of tasks that can be queued. Must be a power of two - see @ref getTaskSize
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
            requires std::is_base_of_v<ThreadPoolJob, T>
        T* PushImpl(Args&&... args)
        {
            if (mShutdown)
                throw std::runtime_error("ThreadPool shutting down");
            auto task = ConstructUniqueBase<ThreadPoolJob, T>(mAllocator, std::forward<Args>(args)...);
            T* ptr = static_cast<T*>(task.get());
            if (!mJobs.Push(std::move(task)))
                throw std::runtime_error("Jobs full");
            mTotal.fetch_add(1, std::memory_order_relaxed);
            mTotal.notify_one();
            return ptr;
        }
        /**
         * @brief Push a lambda job to the thread pool.
         * @return @ref Future<func ReturnType> that will be set when the job is completed.
         */
        template <typename Lambda, typename... Args>
        auto Push(Lambda&& func, Args const&... args)
        {
            auto LambdaFn = [func = std::forward<Lambda>(func), ... args = args] { return func(args...); };
            using LambdaType = decltype(LambdaFn);
            using ReturnType = decltype(LambdaFn());
            ThreadPoolLambdaJob<LambdaType, ReturnType> job(std::forward<LambdaType>(LambdaFn));
            auto fut = job.mPromise.get_future();
            PushImpl<ThreadPoolLambdaJob<LambdaType, ReturnType>>(std::move(job));
            return std::move(fut);
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
        const static size_t getTaskSize(size_t size) { return 1ULL << static_cast<size_t>(std::ceil(std::log2f(size))); }
    };
} // namespace Foundation::Core
