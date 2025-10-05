#include "ThreadPool.hpp"
#include <tracy/TracyC.h>
namespace Foundation::Async
{
    ThreadPool::ThreadPool(size_t numThreads, size_t maxTasks, Allocator* alloc, StringView name):
        mAllocator(alloc), mName(name), mJobs(maxTasks, alloc),
        mJobsWriter(mJobs.CreateWriter()), mThreads(alloc)
    {
        for (size_t i = 0; i < numThreads; ++i)
            mThreads.emplace_back(&ThreadPool::ThreadPoolWorker, this, i);
    }
    void ThreadPool::Shutdown()
    {
        mShutdown = true;
        mTotal.store(-1LL, std::memory_order_relaxed);
        mTotal.notify_all();
    }
    void ThreadPool::Join()
    {
        size_t complete = mComplete.load(std::memory_order_relaxed);
        while (complete < mTotal.load(std::memory_order_relaxed))
        {
            mComplete.wait(complete);
            complete = mComplete.load(std::memory_order_relaxed);
        }
    }
    ThreadPool::~ThreadPool()
    {
        Shutdown();
    }
    void ThreadPool::ThreadPoolWorker(size_t id)
    {
        TracyCSetThreadName(fmt::format("{}@{}", mName.c_str(), id).c_str());
        auto reader = mJobs.CreateReader();
        size_t total = 0;
        while (!mShutdown)
        {
            // Atomically wait on the counter.
            // Different OSes do this differently. For example MSVC's STL does this through
            // WaitOnAddress [https://github.com/microsoft/STL/blob/43e96b29a971a7a26aceaa539d22cbedfcf13687/stl/src/atomic_wait.cpp#L99]
            // While it's futex [https://man7.org/linux/man-pages/man2/futex.2.html] on POSIX systems.
            mTotal.wait(total, std::memory_order_relaxed);
            // Increment our local counter
            // We'd busy wait for at most actual `m_total` cycles per thread
            // before letting the OS primitive take over
            total++;
            UniquePtr<ThreadPoolJob> job;
            if (reader.Pop(job))
            {
                job->Execute(id);
                mComplete.fetch_add(1, std::memory_order_relaxed);
                mComplete.notify_one();
            }
        }
    }
} // namespace Foundation::Async
