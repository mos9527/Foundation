#include "ThreadPool.hpp"
#include <tracy/TracyC.h>
namespace Foundation::Core
{
    namespace
    {
        thread_local ThreadPool* gWorkerThreadPool{};

        Allocator* ValidateThreadPoolDesc(Allocator* allocator, size_t maxTasks)
        {
            if (!allocator)
                throw std::runtime_error("ThreadPool requires an allocator");
            if (!std::has_single_bit(maxTasks))
                throw std::runtime_error("ThreadPool maxTasks must be a non-zero power of two");
            return allocator;
        }
    }

    ThreadPool::ThreadPool(size_t numThreads, size_t maxTasks, Allocator* alloc, StringView name) :
        mAllocator(ValidateThreadPoolDesc(alloc, maxTasks)), mName(name),
        mJobs{JobQueue(maxTasks, mAllocator), JobQueue(maxTasks, mAllocator), JobQueue(maxTasks, mAllocator)},
        mThreads(mAllocator)
    {
        try
        {
            mThreads.reserve(numThreads);
            for (size_t i = 0; i < numThreads; ++i)
                mThreads.emplace_back(&ThreadPool::ThreadPoolWorker, this, i);
        }
        catch (...)
        {
            mShutdown.store(true, std::memory_order_release);
            mWakeEpoch.fetch_add(1, std::memory_order_release);
            mWakeEpoch.notify_all();
            for (Thread& thread : mThreads)
                if (thread.joinable())
                    thread.join();
            throw;
        }
    }
    void ThreadPool::Shutdown()
    {
        if (gWorkerThreadPool == this)
            throw std::runtime_error("ThreadPool cannot be shut down from one of its jobs");
        {
            std::unique_lock submitLock(mSubmitMutex);
            if (!mAccepting.load(std::memory_order_relaxed))
            {
                submitLock.unlock();
                bool shutdown = mShutdown.load(std::memory_order_acquire);
                while (!shutdown)
                {
                    mShutdown.wait(false, std::memory_order_relaxed);
                    shutdown = mShutdown.load(std::memory_order_acquire);
                }
                return;
            }
            mAccepting.store(false, std::memory_order_release);
            mSubmitCV.wait(submitLock, [this] { return mSubmitting == 0; });
        }
        Join();
        mShutdown.store(true, std::memory_order_release);
        mShutdown.notify_all();
        mWakeEpoch.fetch_add(1, std::memory_order_release);
        mWakeEpoch.notify_all();
    }
    void ThreadPool::Join()
    {
        while (mComplete.load(std::memory_order_acquire) < mTotal.load(std::memory_order_acquire))
        {
            size_t const progress = mProgressEpoch.load(std::memory_order_acquire);
            if (mComplete.load(std::memory_order_acquire) < mTotal.load(std::memory_order_acquire))
                mProgressEpoch.wait(progress, std::memory_order_relaxed);
        }
    }
    ThreadPool::~ThreadPool()
    {
        Shutdown();
        for (auto& t : mThreads)
        {
            if (t.joinable())
                t.join();
        }
    }
    void ThreadPool::ThreadPoolWorker(size_t id)
    {
        TracyCSetThreadName(fmt::format("{}@{}", mName.c_str(), id).c_str());
        gWorkerThreadPool = this;
        while (!mShutdown.load(std::memory_order_acquire))
        {
            size_t const epoch = mWakeEpoch.load(std::memory_order_acquire);
            UniquePtr<Job> job;
            for (size_t priority = kJobPriorityCount; priority-- > 0;)
            {
                if (mJobs[priority].Pop(job))
                {
                    job->Execute(id);
                    mComplete.fetch_add(1, std::memory_order_release);
                    mProgressEpoch.fetch_add(1, std::memory_order_release);
                    mProgressEpoch.notify_all();
                    break;
                }
            }
            if (job)
                continue;
            if (!mShutdown.load(std::memory_order_acquire))
                mWakeEpoch.wait(epoch, std::memory_order_relaxed);
        }
        gWorkerThreadPool = nullptr;
    }
} // namespace Foundation::Core
