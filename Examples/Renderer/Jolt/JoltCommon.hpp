#pragma once
#include <Core/JobSystem.hpp>
#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <cassert>

namespace Foundation::Examples
{
    class FoundationJoltJobSystem final : public JPH::JobSystemWithBarrier
    {
        using JoltJob = JPH::JobSystem::Job;

        struct QueuedJob
        {
            JoltJob* job{};

            explicit QueuedJob(JoltJob* value) : job(value) { job->AddRef(); }
            QueuedJob(QueuedJob const&) = delete;
            QueuedJob& operator=(QueuedJob const&) = delete;
            QueuedJob(QueuedJob&& other) noexcept : job(std::exchange(other.job, nullptr)) {}
            QueuedJob& operator=(QueuedJob&&) = delete;
            ~QueuedJob()
            {
                if (job)
                    job->Release();
            }

            void operator()() const { job->Execute(); }
        };

        Core::Allocator* mAllocator;
        size_t mMaxJobs;
        Core::ScopedArena mJobArena;
        Core::Vector<uint32_t> mFreelist;
        Core::Mutex mFreelistMutex;
        Core::JobSystem mJobs;

        static size_t WorkerCount(int threadCount)
        {
            if (threadCount >= 0)
                return static_cast<size_t>(threadCount);
            size_t const hardware = std::thread::hardware_concurrency();
            return hardware > 1 ? hardware - 1 : 0;
        }

        JoltJob* JobAt(uint32_t index) const noexcept
        {
            auto* memory = static_cast<std::byte*>(mJobArena.arena.memory);
            return reinterpret_cast<JoltJob*>(memory + static_cast<size_t>(index) * sizeof(JoltJob));
        }

    public:
        FoundationJoltJobSystem(uint32_t maxJobs, uint32_t maxBarriers, int threadCount = -1,
                                Core::Allocator* allocator = GLOBAL_ALLOC) :
            JPH::JobSystemWithBarrier(maxBarriers),
            mAllocator(allocator),
            mMaxJobs(maxJobs),
            mJobArena(mAllocator, static_cast<size_t>(maxJobs) * sizeof(JoltJob), alignof(JoltJob)),
            mFreelist(mAllocator),
            mJobs(Core::JobSystemDesc{
                .workerCount = WorkerCount(threadCount),
                .maxJobs = maxJobs,
                .maxBarriers = 1,
                .readyQueueSize = Core::ThreadPool::CalcTaskSize(maxJobs),
                .allocator = mAllocator,
                .name = "Jolt",
            })
        {
            CHECK(mJobArena);
            mFreelist.reserve(maxJobs);
            for (uint32_t i = maxJobs; i-- > 0;)
                mFreelist.push_back(i);
        }

        ~FoundationJoltJobSystem() override
        {
            mJobs.Drain();
            assert(mFreelist.size() == mMaxJobs);
        }

        int GetMaxConcurrency() const override { return static_cast<int>(mJobs.GetMaxConcurrency()); }

        JPH::JobHandle CreateJob(char const* name, JPH::ColorArg color, JobFunction const& function,
                                 JPH::uint32 dependencyCount = 0) override
        {
            uint32_t index;
            {
                std::lock_guard lock(mFreelistMutex);
                CHECK(!mFreelist.empty());
                index = mFreelist.back();
                mFreelist.pop_back();
            }

            JoltJob* job;
            try
            {                
                job = std::construct_at(JobAt(index), name, color, this, function, dependencyCount);
            }
            catch (...)
            {
                std::lock_guard lock(mFreelistMutex);
                mFreelist.push_back(index);
                throw;
            }

            JPH::JobHandle handle(job);
            if (dependencyCount == 0)
                QueueJob(job);
            return handle;
        }

        void WaitForJobs(Barrier* barrier) override
        {
            JPH::JobSystemWithBarrier::WaitForJobs(barrier);
            mJobs.Drain();
        }

    protected:
        void QueueJob(JoltJob* job) override
        {
            char const* name = "Jolt";
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
            name = job->GetName();
#endif
            mJobs.CreateJob(name, Core::JobPriority::Normal, QueuedJob(job));
        }

        void QueueJobs(JoltJob** jobs, JPH::uint count) override
        {
            for (JPH::uint i = 0; i < count; ++i)
                QueueJob(jobs[i]);
        }

        void FreeJob(JoltJob* job) override
        {
            auto* begin = static_cast<std::byte*>(mJobArena.arena.memory);
            auto* pointer = reinterpret_cast<std::byte*>(job);
            size_t const byteOffset = static_cast<size_t>(pointer - begin);
            uint32_t const index = static_cast<uint32_t>(byteOffset / sizeof(JoltJob));
            job->~JoltJob();
            std::lock_guard lock(mFreelistMutex);
            mFreelist.push_back(index);
        }
    };
} // namespace Foundation::Examples
