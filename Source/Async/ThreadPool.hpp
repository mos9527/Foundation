#pragma once
#include <Bits/Ranges.hpp>
#include <Core/Core.hpp>
#include "Future.hpp"
#include "Thread.hpp"
namespace Foundation::Async
{
    using namespace Core;
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
    class ThreadPool
    {
        Allocator* m_allocator;
        Vector<Thread> m_threads; // All worker threads
        Vector<Pair<int, UniquePtr<ThreadPoolJob>>> m_jobs; // Priority, Job

        bool m_shutdown{false};
        Mutex m_mutex;
        Condition m_jobCond;

        void ThreadPoolWorker(size_t id);
        void PushJob(size_t priority, UniquePtr<ThreadPoolJob>&& job);
        UniquePtr<ThreadPoolJob> PopJob();
    public:
        /**
         * @brief Construct a thread pool with the given number of worker threads.
         * @param numThreads Number of worker threads to spawn.
         * @param alloc Allocator to use for internal and job allocations
         */
        ThreadPool(size_t numThreads, Allocator* alloc);
        /**
         * @brief Construct a thread pool with all available hardware thread count.
         * @param alloc Allocator to use for internal and job allocations
         */
        ThreadPool(Allocator* alloc) : ThreadPool(std::thread::hardware_concurrency(), alloc) {};
        /**
         * @brief Push a job with the given priority to the thread pool.
         * @param priority Priority of the job. Higher priority jobs are executed first.
         * @return @ref SharedPromise that will be set when the job is completed.
         */
        template <typename Lambda, typename... Args>
        auto PushPriority(Lambda&& func, int priority, Args&&... args)
        {
            CHECK(!m_shutdown);
            auto packaged = ThreadPoolPackagedLambda(std::forward<Lambda>(func), std::forward<Args>(args)...);
            using ReturnType = decltype(func(args...));
            using PackagedType = decltype(packaged);
            // Use the wrapped lambda type for the job
            using LambdaType = ThreadPoolLambdaJob<PackagedType, ReturnType>;
            // Construct the job on our own allocator
            auto promise = ConstructShared<std::promise<ReturnType>>(m_allocator);
            std::scoped_lock lock(m_mutex);
            PushJob(priority,
                    ConstructUniqueBase<ThreadPoolJob, LambdaType>(m_allocator, promise,
                                                                   std::forward<PackagedType>(packaged)));
            return promise;
        }
        /**
         * @brief Push a job with default priority (0) to the thread pool.
         * @return @ref SharedPromise that will be set when the job is completed.
         */
        template <typename Lambda, typename... Args>
        auto Push(Lambda&& func, Args&&... args)
        {
            return PushPriority(std::forward<Lambda>(func), 0, std::forward<Args>(args)...);
        }
        void Shutdown() { m_shutdown = true; }
        ~ThreadPool();
    };
} // namespace Foundation::Async
