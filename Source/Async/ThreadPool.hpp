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
    struct ThreadPoolJob
    {
        virtual ~ThreadPoolJob() = default;
        virtual void Execute() noexcept = 0;
    };
    inline auto ThreadPoolPackagedLambda = []<typename T0, typename... T1>(T0&& func, T1&&... args)
    {
        return [func = std::forward<T0>(func), ... args = std::forward<T1>(args)]
        { return func(std::forward<decltype(args)>(args)...); };
    };
    template <typename Lambda, typename ReturnType>
    class ThreadPoolLambdaJob final : public ThreadPoolJob
    {
        Lambda m_func;
        SharedPromise<ReturnType> m_promise;
    public:
        ThreadPoolLambdaJob(SharedPromise<ReturnType> promise, Lambda&& func) : m_func(func), m_promise(promise) {}
        void Execute() noexcept override
        {
            try
            {
                if constexpr (std::is_same_v<ReturnType, void>)
                {
                    m_func();
                    m_promise->set_value();
                }
                else
                    m_promise->set_value(m_func());
            }
            catch (...)
            {
                m_promise->set_exception(std::current_exception());
            }
        }
    };
    using JobQueue = MPMCQueue<UniquePtr<ThreadPoolJob>>;
    /**
     * @breif Atomic, lock-free Thread Pool implementation with fixed bounds
     */
    class ThreadPool
    {
        Allocator* m_allocator;
        Vector<Thread> m_threads;

        Atomic<bool> m_shutdown{false};
        Atomic<size_t> m_complete{ 0 };
        Atomic<size_t> m_total{ 0 };

        JobQueue m_jobs;
        JobQueue::Writer m_jobsWriter;

        void ThreadPoolWorker(size_t id);
    public:
        /**
         * @brief Construct a thread pool with the given number of worker threads.
         * @param numThreads Number of worker threads to spawn.
         * @param maxTasks Max number of tasks that can be queued. Must be a power of two.
         * @param alloc Allocator to use for internal and job allocations
         */
        ThreadPool(size_t numThreads, size_t maxTasks, Allocator* alloc);
        /**
         * @brief Push a lambda job to the thread pool.
         * @return @ref SharedPromise that will be set when the job is completed.
         */
        template <typename Lambda, typename... Args>
        auto Push(Lambda&& func, Args&&... args)
        {
            CHECK_MSG(!m_shutdown, "ThreadPool shutting down");
            auto packaged = ThreadPoolPackagedLambda(std::forward<Lambda>(func), std::forward<Args>(args)...);
            using ReturnType = decltype(func(args...));
            using PackagedType = decltype(packaged);
            // Use the wrapped lambda type for the job
            using LambdaType = ThreadPoolLambdaJob<PackagedType, ReturnType>;
            auto promise = ConstructShared<std::promise<ReturnType>>(m_allocator);
            CHECK_MSG(m_jobsWriter.push(
                ConstructUniqueBase<ThreadPoolJob, LambdaType>(
                    m_allocator, promise,
                    std::forward<PackagedType>(packaged))), "Jobs full");
            m_total.fetch_add(1, std::memory_order_relaxed);
            m_total.notify_one();
            return promise;
        }
        /**
         * @brief Shutdown the @ref ThreadPool, potentially cancelling all pending jobs.
         * @note This does not cancel running jobs.
         * @note Calling @ref Join _after_ this has no effect.
         */
        void Shutdown();
        /**
         * @brief Wait for all scheduled jobs to complete
         * @note This is always automatically called on destruction. For forceful shutdowns,
         *       call @ref Shutdown before destructing the @ref Threadpool
         */
        void Join();
        ~ThreadPool();
    };
} // namespace Foundation::Async
