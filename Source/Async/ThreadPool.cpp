#include "ThreadPool.hpp"
#include <tracy/TracyC.h>
namespace Foundation::Async
{
    ThreadPool::ThreadPool(size_t numThreads, size_t maxTasks, Allocator* alloc, StringView name):
        m_allocator(alloc), m_name(name), m_jobs(maxTasks, alloc),
        m_jobsWriter(m_jobs.create_writer()), m_threads(alloc)
    {
        for (size_t i = 0; i < numThreads; ++i)
            m_threads.emplace_back(&ThreadPool::ThreadPoolWorker, this, i);
    }
    void ThreadPool::Shutdown()
    {
        m_shutdown = true;
        m_total.store(-1, std::memory_order_relaxed);
        m_total.notify_all();
    }
    void ThreadPool::Join()
    {
        size_t complete = m_complete.load(std::memory_order_relaxed);
        while (complete < m_total.load(std::memory_order_relaxed))
        {
            m_complete.wait(complete);
            complete = m_complete.load(std::memory_order_relaxed);
        }
    }
    ThreadPool::~ThreadPool()
    {
        Shutdown();
    }
    void ThreadPool::ThreadPoolWorker(size_t id)
    {
        TracyCSetThreadName(fmt::format("{}@{}", m_name.c_str(), id).c_str());
        auto reader = m_jobs.create_reader();
        size_t total = 0;
        while (!m_shutdown)
        {
            // Atomically wait on the counter.
            // Different OSes do this differently. For example MSVC's STL does this through
            // WaitOnAddress [https://github.com/microsoft/STL/blob/43e96b29a971a7a26aceaa539d22cbedfcf13687/stl/src/atomic_wait.cpp#L99]
            // While it's futex [https://man7.org/linux/man-pages/man2/futex.2.html] on POSIX systems.
            m_total.wait(total, std::memory_order_relaxed);
            // Increment our local counter
            // We'd busy wait for at most actual `m_total` cycles per thread
            // before letting the OS primitive take over
            total++;
            UniquePtr<ThreadPoolJob> job;
            if (reader.pop(job))
            {
                job->Execute(id);
                m_complete.fetch_add(1, std::memory_order_relaxed);
                m_complete.notify_one();
            }
        }
    }
} // namespace Foundation::Async
