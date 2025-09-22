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
    void ThreadPool::PushJob(size_t priority, UniquePtr<ThreadPoolJob>&& job)
    {
        m_jobs.emplace_back(priority, std::move(job));
        Bits::Ranges::push_heap(m_jobs, [](auto const& a, auto const& b) { return a.first < b.first; });
        m_jobCond.notify_one();
    }
    UniquePtr<ThreadPoolJob> ThreadPool::PopJob()
    {
        Bits::Ranges::pop_heap(m_jobs);
        UniquePtr<ThreadPoolJob> job = std::move(m_jobs.back().second);
        m_jobs.pop_back();
        return std::move(job);
    }
    void ThreadPool::ThreadPoolWorker(size_t id)
    {
        while (!m_shutdown)
        {
            UniquePtr<ThreadPoolJob> job;
            {
                std::unique_lock lock(m_mutex);
                m_jobCond.wait(lock, [this] { return m_shutdown || !m_jobs.empty(); });
                LOG_RUNTIME(ThreadPoolWorker, info, "Thread {} wake up", id);
                if (!m_jobs.empty())
                    job = PopJob();
                lock.unlock();
            }
            if (job)
                job->Execute();
        }
    }
} // namespace Foundation::Async
