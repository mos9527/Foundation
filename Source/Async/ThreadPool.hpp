#pragma once
#include <Atomics/Queue.hpp>
#include <Bits/Ranges.hpp>
#include <Core/Core.hpp>
#include "Future.hpp"
#include "Thread.hpp"
namespace Foundation::Async
{
    using namespace Core;
    using namespace Async;
    using namespace Atomics;
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
    template <typename Lambda, typename ReturnType>
    class ThreadPoolLambdaJob final : public ThreadPoolJob
    {
        Lambda mFunc;
        SharedPromise<ReturnType> mPromise;
    public:
        ThreadPoolLambdaJob(SharedPromise<ReturnType> promise, Lambda&& func) : mFunc(func), mPromise(promise) {}
        void Execute(size_t) noexcept override
        {
            try
            {
                if constexpr (std::is_same_v<ReturnType, void>)
                {
                    mFunc();
                    mPromise->set_value();
                }
                else
                    mPromise->set_value(mFunc());
            }
            catch (...)
            {
                mPromise->set_exception(std::current_exception());
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
        Atomic<size_t> mComplete{ 0 };
        Atomic<size_t> mTotal{ 0 };

        JobQueue mJobs;
        JobQueue::Writer mJobsWriter;
        // Ensure threads are joined first on destruction
        Vector<Thread> mThreads;
        void ThreadPoolWorker(size_t id);
    public:
        /**
         * @brief Construct a thread pool with the given number of worker threads.
         * @param numThreads Number of worker threads to spawn.
         * @param maxTasks Max number of tasks that can be queued. Must be a power of two.
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
        template <typename T, typename ... Args>
        requires std::is_base_of_v<ThreadPoolJob, T>
        void PushImpl(Args&&... args)
        {
            CHECK_MSG(!mShutdown, "ThreadPool shutting down");
            CHECK_MSG(mJobsWriter.Push(
                ConstructUniqueBase<ThreadPoolJob, T>(mAllocator, std::forward<Args>(args)...)), "Jobs full");
            mTotal.fetch_add(1, std::memory_order_relaxed);
            mTotal.notify_one();
        }
        /**
         * @brief Push a lambda job to the thread pool.
         * @return @ref SharedPromise that will be set when the job is completed.
         */
        template <typename Lambda, typename... Args>
        auto Push(Lambda&& func, Args&&... args)
        {
            CHECK_MSG(!mShutdown, "ThreadPool shutting down");
            auto ThreadPoolPackagedLambda = [](Lambda&& fn, Args&&... fargs)
            {
                return [func = std::forward<Lambda>(fn), ... fargs = std::forward<Args>(fargs)]
                { return func(std::forward<Args>(fargs)...); };
            };
            auto packaged = ThreadPoolPackagedLambda(std::forward<Lambda>(func), std::forward<Args>(args)...);
            using ReturnType = decltype(func(args...));
            using PackagedType = decltype(packaged);
            // Use the wrapped lambda type for the job
            using LambdaType = ThreadPoolLambdaJob<PackagedType, ReturnType>;
            auto promise = ConstructShared<std::promise<ReturnType>>(mAllocator);
            CHECK_MSG(mJobsWriter.Push(
                ConstructUniqueBase<ThreadPoolJob, LambdaType>(
                    mAllocator, promise,
                    std::forward<PackagedType>(packaged))), "Jobs full");
            mTotal.fetch_add(1, std::memory_order_relaxed);
            mTotal.notify_one();
            return promise;
        }
        /**
         * @brief Shutdown the @ref ThreadPool, potentially cancelling all pending jobs.
         * @note This does not cancel running jobs.
         */
        void Shutdown();
        /**
         * @brief Wait for all scheduled jobs to complete
         */
        void Join();
        ~ThreadPool();
    };
} // namespace Foundation::Async
