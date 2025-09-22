#include "ThreadPool.hpp"
namespace Foundation::Async
{
    ThreadPool::ThreadPool(size_t numThreads, Allocator* alloc) :
        m_allocator(alloc), m_threads(alloc), m_jobs(alloc)
    {
        for (size_t i = 0; i < numThreads; ++i)
            m_threads.emplace_back(&ThreadPool::ThreadPoolWorker, this, i);
    }
    ThreadPool::~ThreadPool()
    {
        Shutdown();
        m_jobCond.notify_all();
        // Automatically join the threads
    }
    void ThreadPool::ThreadPoolWorker(size_t id)
    {
        while (!m_shutdown)
        {
            UniquePtr<ThreadPoolJob> job{};
            {
                std::unique_lock lock(m_jobMutex);
                m_jobCond.wait(lock, [this] { return m_shutdown; });
                job.reset();
                if (!m_jobs.empty())
                {
                    LOG_RUNTIME(ThreadPoolWorker, info, "{} got a job", id);
                    job = std::move(m_jobs.back().second);
                    Bits::Ranges::pop_heap(m_jobs);
                }
                lock.unlock();
            }
            if (job)
                job->Execute();
        }
    }
} // namespace Foundation::Async
