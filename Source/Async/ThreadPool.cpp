#include "ThreadPool.hpp"
namespace Foundation::Async
{
    ThreadPool::ThreadPool(size_t numThreads, size_t maxTasks, Allocator* alloc) :
        m_allocator(alloc), m_threads(alloc), m_jobs(maxTasks, alloc),
        m_jobsWriter(m_jobs.create_writer())
    {
        for (size_t i = 0; i < numThreads; ++i)
            m_threads.emplace_back(&ThreadPool::ThreadPoolWorker, this, i);
    }
    void ThreadPool::Join()
    {
        while (m_complete.load(std::memory_order_relaxed) < m_total.load(std::memory_order_relaxed))
            std::this_thread::yield();
    }
    ThreadPool::~ThreadPool()
    {
        Shutdown();
        Join();
    }
    void ThreadPool::ThreadPoolWorker(size_t id)
    {
        auto reader = m_jobs.create_reader();
        while (!m_shutdown)
        {
            UniquePtr<ThreadPoolJob> job;
            if (reader.pop(job))
            {
                job->Execute();
                m_complete.fetch_add(1, std::memory_order_relaxed);
            } else
                std::this_thread::yield();
        }
    }
} // namespace Foundation::Async
