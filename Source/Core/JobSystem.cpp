#include "JobSystem.hpp"
#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

namespace Foundation::Core
{
    namespace
    {
        thread_local JobSystem* gExecutingJobSystem{};

        bool IsTerminal(JobStatus status) noexcept
        {
            return status == JobStatus::Completed || status == JobStatus::Failed || status == JobStatus::Cancelled;
        }

        Allocator* ValidateDesc(JobSystemDesc const& desc)
        {
            CHECK_MSG(desc.allocator, "JobSystem requires an allocator");
            CHECK_MSG(desc.maxJobs != 0 && desc.maxBarriers != 0, "JobSystem capacities must be non-zero");
            CHECK_MSG(std::has_single_bit(desc.readyQueueSize) && desc.readyQueueSize >= desc.maxJobs,
                      "JobSystem ready queue must be a power of two and hold maxJobs entries");
            return desc.allocator;
        }
    }

    struct JobBarrierState
    {
        Mutex mutex;
        Vector<JobHandle> jobs;
        Atomic<size_t> pending{0};
        bool waiting{};

        explicit JobBarrierState(Allocator* allocator) : jobs(allocator) {}

        void Complete() noexcept
        {
            size_t const previous = pending.fetch_sub(1, std::memory_order_release);
            CHECK(previous > 0);
            pending.notify_all();
        }
    };

    struct JobNode
    {
        Allocator* allocator;
        String name;
        JobPriority priority{JobPriority::Normal};
        UniquePtr<JobCallable> callable;
        Atomic<JobStatus> status{JobStatus::Cancelled};
        Atomic<bool> cancellation{false};
        Atomic<uint32_t> references{0};
        Atomic<uint32_t> generation{0};
        Atomic<bool> active{false};
        Mutex completionMutex;
        uint32_t dependencies{};
        bool prerequisiteFailed{};
        Vector<JobHandle> successors;
        Vector<WeakPtr<JobBarrierState>> barriers;

        explicit JobNode(Allocator* alloc) :
            allocator(alloc), callable(nullptr, StlDeleter<JobCallable>{nullptr}), successors(alloc), barriers(alloc)
        {
        }

        void Initialize(StringView jobName, JobPriority jobPriority, UniquePtr<JobCallable> jobCallable,
                        uint32_t dependencyCount)
        {
            name = jobName;
            priority = jobPriority;
            callable = std::move(jobCallable);
            dependencies = dependencyCount;
            prerequisiteFailed = false;
            cancellation.store(false, std::memory_order_relaxed);
            successors.clear();
            barriers.clear();
            status.store(JobStatus::Waiting, std::memory_order_release);
        }

        void Reset()
        {
            callable.reset();
            name.clear();
            successors.clear();
            barriers.clear();
            dependencies = 0;
            prerequisiteFailed = false;
            cancellation.store(false, std::memory_order_relaxed);
            status.store(JobStatus::Cancelled, std::memory_order_relaxed);
        }
    };

    struct JobPool
    {
        Allocator* allocator;
        Span<JobNode> nodes;
        Vector<uint32_t> freelist;
        Mutex mutex;
        Mutex ownerMutex;
        CondVar ownerCV;
        JobSystem* owner{};
        size_t ownerUsers{};
        Atomic<size_t> liveBarriers{0};

        JobPool(Allocator* alloc, size_t maxJobs) :
            allocator(alloc), nodes(ConstructSpan<JobNode>(alloc, maxJobs, alloc)), freelist(alloc)
        {
            freelist.reserve(maxJobs);
            for (size_t i = maxJobs; i-- > 0;)
                freelist.push_back(static_cast<uint32_t>(i));
        }

        ~JobPool()
        {
            CHECK(freelist.size() == nodes.size());
            DestructSpan(allocator, nodes);
        }

        void Attach(JobSystem* system)
        {
            std::lock_guard lock(ownerMutex);
            CHECK(owner == nullptr);
            owner = system;
        }

        JobSystem* AcquireOwner()
        {
            std::lock_guard lock(ownerMutex);
            if (!owner)
                return nullptr;
            ++ownerUsers;
            return owner;
        }

        void ReleaseOwner()
        {
            std::lock_guard lock(ownerMutex);
            CHECK(ownerUsers > 0);
            if (--ownerUsers == 0)
                ownerCV.notify_all();
        }

        void Detach()
        {
            std::unique_lock lock(ownerMutex);
            owner = nullptr;
            ownerCV.wait(lock, [this] { return ownerUsers == 0; });
        }

        Pair<uint32_t, uint32_t> Allocate(StringView name, JobPriority priority, UniquePtr<JobCallable> callable,
                                          uint32_t dependencies)
        {
            std::lock_guard lock(mutex);
            CHECK_MSG(!freelist.empty(), "JobSystem job capacity exhausted");
            uint32_t const index = freelist.back();
            freelist.pop_back();
            JobNode& node = nodes[index];
            uint32_t const generation = node.generation.fetch_add(1, std::memory_order_relaxed) + 1;
            node.Initialize(name, priority, std::move(callable), dependencies);
            node.references.store(2, std::memory_order_release); // system registry + returned handle
            node.active.store(true, std::memory_order_release);
            return {index, generation};
        }

        [[nodiscard]] bool Validate(uint32_t index, uint32_t generation) const noexcept
        {
            return index < nodes.size() && nodes[index].active.load(std::memory_order_acquire) &&
                nodes[index].generation.load(std::memory_order_acquire) == generation;
        }

        JobNode* Get(uint32_t index, uint32_t generation) noexcept
        {
            return Validate(index, generation) ? &nodes[index] : nullptr;
        }

        void AddRef(uint32_t index, uint32_t generation) noexcept
        {
            CHECK(Validate(index, generation));
            nodes[index].references.fetch_add(1, std::memory_order_relaxed);
        }

        void Release(uint32_t index, uint32_t generation) noexcept
        {
            if (!Validate(index, generation))
                return;
            JobNode& node = nodes[index];
            if (node.references.fetch_sub(1, std::memory_order_acq_rel) != 1)
                return;

            std::lock_guard lock(mutex);
            CHECK(node.generation.load(std::memory_order_relaxed) == generation);
            CHECK(IsTerminal(node.status.load(std::memory_order_acquire)));
            node.Reset();
            node.active.store(false, std::memory_order_release);
            freelist.push_back(index);
        }

        JobHandle AcquireActive(SharedPtr<JobPool> const& self, uint32_t index)
        {
            std::lock_guard lock(mutex);
            if (index >= nodes.size() || !nodes[index].active.load(std::memory_order_acquire))
                return {};
            JobNode& node = nodes[index];
            uint32_t references = node.references.load(std::memory_order_acquire);
            while (references != 0 &&
                   !node.references.compare_exchange_weak(references, references + 1, std::memory_order_acquire,
                                                          std::memory_order_relaxed))
            {
            }
            if (references == 0)
                return {};
            return JobHandle(self, index, node.generation.load(std::memory_order_relaxed), JobHandle::AdoptRef{});
        }
    };

    JobHandle::JobHandle(SharedPtr<JobPool> pool, uint32_t index, uint32_t generation, AdoptRef) noexcept :
        mPool(std::move(pool)), mIndex(index), mGeneration(generation)
    {
    }

    JobHandle::JobHandle(JobHandle const& other) noexcept :
        mPool(other.mPool), mIndex(other.mIndex), mGeneration(other.mGeneration)
    {
        AddRef();
    }

    JobHandle& JobHandle::operator=(JobHandle const& other) noexcept
    {
        if (this == &other)
            return *this;
        JobHandle copy(other);
        *this = std::move(copy);
        return *this;
    }

    JobHandle::JobHandle(JobHandle&& other) noexcept :
        mPool(std::move(other.mPool)), mIndex(std::exchange(other.mIndex, UINT32_MAX)),
        mGeneration(std::exchange(other.mGeneration, 0))
    {
    }

    JobHandle& JobHandle::operator=(JobHandle&& other) noexcept
    {
        if (this == &other)
            return *this;
        Release();
        mPool = std::move(other.mPool);
        mIndex = std::exchange(other.mIndex, UINT32_MAX);
        mGeneration = std::exchange(other.mGeneration, 0);
        return *this;
    }

    JobHandle::~JobHandle() { Release(); }

    void JobHandle::AddRef() const noexcept
    {
        if (mPool && mIndex != UINT32_MAX)
            mPool->AddRef(mIndex, mGeneration);
    }

    void JobHandle::Release() noexcept
    {
        if (mPool && mIndex != UINT32_MAX)
            mPool->Release(mIndex, mGeneration);
        mPool.reset();
        mIndex = UINT32_MAX;
        mGeneration = 0;
    }

    bool JobHandle::IsValid() const noexcept
    {
        return mPool && mPool->Validate(mIndex, mGeneration);
    }

    JobStatus JobHandle::Status() const noexcept
    {
        if (!IsValid())
            return JobStatus::Cancelled;
        return mPool->nodes[mIndex].status.load(std::memory_order_acquire);
    }

    bool JobHandle::IsDone() const noexcept { return IsTerminal(Status()); }

    void JobHandle::AddDependency(uint32_t count) const
    {
        CHECK_MSG(IsValid() && count != 0, "Invalid job dependency");
        JobSystem* system = mPool->AcquireOwner();
        CHECK_MSG(system, "JobSystem no longer exists");
        JobNode& node = mPool->nodes[mIndex];
        std::lock_guard lock(node.completionMutex);
        CHECK_MSG(node.status.load(std::memory_order_relaxed) == JobStatus::Waiting,
                  "Cannot add a dependency to a queued or completed job");
        CHECK_MSG(node.dependencies <= std::numeric_limits<uint32_t>::max() - count,
                  "Job dependency counter overflow");
        node.dependencies += count;
        mPool->ReleaseOwner();
    }

    void JobHandle::RemoveDependency(uint32_t count) const
    {
        CHECK_MSG(IsValid() && count != 0, "Invalid job dependency");
        JobSystem* system = mPool->AcquireOwner();
        CHECK_MSG(system, "JobSystem no longer exists");
        system->ReleaseDependency(*this, count, false);
        mPool->ReleaseOwner();
    }

    JobBarrier::JobBarrier(JobBarrier&& other) noexcept :
        mPool(std::move(other.mPool)), mState(std::move(other.mState))
    {
    }

    JobBarrier& JobBarrier::operator=(JobBarrier&& other) noexcept
    {
        if (this == &other)
            return *this;
        Reset();
        mPool = std::move(other.mPool);
        mState = std::move(other.mState);
        return *this;
    }

    JobBarrier::~JobBarrier() { Reset(); }

    void JobBarrier::Reset() noexcept
    {
        if (!mState)
            return;
        CHECK(IsEmpty());
        mPool->liveBarriers.fetch_sub(1, std::memory_order_release);
        mPool->liveBarriers.notify_all();
        mState.reset();
        mPool.reset();
    }

    void JobBarrier::Add(JobHandle const& handle)
    {
        CHECK_MSG(mState && handle.IsValid() && handle.mPool.get() == mPool.get(),
                  "Invalid or foreign job barrier add");
        if (!mState || !handle.IsValid() || handle.mPool.get() != mPool.get())
            return;
        JobSystem* owner = mPool->AcquireOwner();
        CHECK_MSG(owner, "JobSystem no longer exists");
        if (!owner)
            return;
        bool const submitting = owner->BeginSubmit();
        CHECK_MSG(submitting, "JobSystem shutting down");
        if (!submitting)
        {
            mPool->ReleaseOwner();
            return;
        }

        bool complete = false;
        {
            std::lock_guard barrierLock(mState->mutex);
            CHECK_MSG(!mState->waiting || mState->pending.load(std::memory_order_acquire) != 0,
                      "Cannot add work while a barrier is completing");
            mState->jobs.push_back(handle);
            mState->pending.fetch_add(1, std::memory_order_acq_rel);
            mState->pending.notify_all();
            JobNode& node = mPool->nodes[handle.mIndex];
            std::lock_guard jobLock(node.completionMutex);
            if (IsTerminal(node.status.load(std::memory_order_acquire)))
                complete = true;
            else
                node.barriers.push_back(mState);
        }
        if (complete)
            mState->Complete();
        owner->EndSubmit();
        mPool->ReleaseOwner();
    }

    void JobBarrier::Add(Span<const JobHandle> jobs)
    {
        for (JobHandle const& job : jobs)
            Add(job);
    }

    bool JobBarrier::IsEmpty() const noexcept
    {
        return !mState || mState->pending.load(std::memory_order_acquire) == 0;
    }

    JobSystem::JobSystem(JobSystemDesc const& desc) :
        mAllocator(ValidateDesc(desc)),
        mName(desc.name), mMaxBarriers(desc.maxBarriers),
        mPool(ConstructShared<JobPool>(mAllocator, mAllocator, desc.maxJobs)),
        mReady{ReadyQueue(desc.readyQueueSize, mAllocator), ReadyQueue(desc.readyQueueSize, mAllocator),
               ReadyQueue(desc.readyQueueSize, mAllocator)},
        mThreads(mAllocator)
    {
        mPool->Attach(this);
        mThreads.reserve(desc.workerCount);
        for (size_t i = 0; i < desc.workerCount; ++i)
            mThreads.emplace_back(&JobSystem::WorkerMain, this, i);
    }

    JobSystem::~JobSystem() { Shutdown(); }

    bool JobSystem::BeginSubmit() noexcept
    {
        std::lock_guard lock(mSubmitMutex);
        if (!mAccepting.load(std::memory_order_relaxed))
            return false;
        ++mSubmitting;
        return true;
    }

    void JobSystem::EndSubmit() noexcept
    {
        std::lock_guard lock(mSubmitMutex);
        CHECK(mSubmitting > 0);
        if (--mSubmitting == 0)
            mSubmitCV.notify_all();
    }

    JobHandle JobSystem::CreateJobInternal(StringView name, JobPriority priority, UniquePtr<JobCallable> callable,
                                           uint32_t dependencyCount)
    {
        CHECK_MSG(PriorityIndex(priority) < kJobPriorityCount, "Invalid job priority");
        auto [index, generation] = mPool->Allocate(name, priority, std::move(callable), dependencyCount);
        JobHandle job(mPool, index, generation, JobHandle::AdoptRef{});
        mOutstanding.fetch_add(1, std::memory_order_release);
        if (dependencyCount == 0)
            Queue(job);
        return job;
    }

    void JobSystem::Queue(JobHandle const& job) noexcept
    {
        JobNode* node = mPool->Get(job.mIndex, job.mGeneration);
        if (!node)
            return;

        bool cancelled = false;
        {
            std::lock_guard lock(node->completionMutex);
            if (node->status.load(std::memory_order_relaxed) != JobStatus::Waiting || node->dependencies != 0)
                return;
            if (node->cancellation.load(std::memory_order_acquire))
                cancelled = true;
            else
                node->status.store(JobStatus::Queued, std::memory_order_release);
        }
        if (cancelled)
        {
            Finish(job, JobStatus::Cancelled);
            return;
        }

        if (!mReady[PriorityIndex(node->priority)].Push(job))
        {
            CHECK_MSG(false, "JobSystem ready queue exhausted");
            Finish(job, JobStatus::Failed);
            return;
        }
        mWakeEpoch.fetch_add(1, std::memory_order_release);
        mWakeEpoch.notify_one();
    }

    void JobSystem::ReleaseDependency(JobHandle const& job, uint32_t count, bool failed)
    {
        CHECK_MSG(job.IsValid() && job.mPool.get() == mPool.get(), "Invalid or foreign job dependency");
        if (!job.IsValid() || job.mPool.get() != mPool.get())
            return;
        JobNode& node = mPool->nodes[job.mIndex];
        bool ready = false;
        bool prerequisiteFailure = false;
        {
            std::lock_guard lock(node.completionMutex);
            if (IsTerminal(node.status.load(std::memory_order_acquire)))
                return;
            CHECK_MSG(node.status.load(std::memory_order_relaxed) == JobStatus::Waiting && node.dependencies >= count,
                      "Job dependency counter underflow");
            node.prerequisiteFailed |= failed;
            node.dependencies -= count;
            ready = node.dependencies == 0;
            prerequisiteFailure = ready && node.prerequisiteFailed;
        }
        if (!ready)
            return;
        if (prerequisiteFailure)
            Finish(job, JobStatus::Failed);
        else
            Queue(job);
    }

    void JobSystem::RegisterSuccessor(JobHandle const& prerequisite, JobHandle const& dependent)
    {
        CHECK_MSG(prerequisite.IsValid() && dependent.IsValid() && prerequisite.mPool.get() == mPool.get() &&
                      dependent.mPool.get() == mPool.get(),
                  "Invalid or foreign job dependency edge");

        JobNode& node = mPool->nodes[prerequisite.mIndex];
        JobStatus status;
        {
            std::lock_guard lock(node.completionMutex);
            status = node.status.load(std::memory_order_acquire);
            if (!IsTerminal(status))
            {
                node.successors.push_back(dependent);
                return;
            }
        }
        ReleaseDependency(dependent, 1, status != JobStatus::Completed);
    }

    void JobSystem::Finish(JobHandle const& job, JobStatus terminalStatus) noexcept
    {
        JobNode* node = mPool->Get(job.mIndex, job.mGeneration);
        if (!node)
            return;
        Vector<JobHandle> successors(mAllocator);
        Vector<WeakPtr<JobBarrierState>> barriers(mAllocator);
        {
            std::lock_guard lock(node->completionMutex);
            JobStatus const current = node->status.load(std::memory_order_acquire);
            if (IsTerminal(current))
                return;
            if (terminalStatus == JobStatus::Cancelled && current == JobStatus::Running)
            {
                node->cancellation.store(true, std::memory_order_release);
                return;
            }
            successors.swap(node->successors);
            barriers.swap(node->barriers);
            node->status.store(terminalStatus, std::memory_order_release);
        }
        node->status.notify_all();

        bool const failed = terminalStatus != JobStatus::Completed;
        for (JobHandle const& successor : successors)
            ReleaseDependency(successor, 1, failed);
        for (WeakPtr<JobBarrierState> const& weakBarrier : barriers)
            if (SharedPtr<JobBarrierState> barrier = weakBarrier.lock())
                barrier->Complete();

        if (mOutstanding.fetch_sub(1, std::memory_order_acq_rel) == 1)
            mOutstanding.notify_all();
        mPool->Release(job.mIndex, job.mGeneration); // system registry reference
    }

    bool JobSystem::TryExecuteOne(size_t workerId)
    {
        JobHandle job;
        for (size_t priority = kJobPriorityCount; priority-- > 0;)
            if (mReady[priority].Pop(job))
                break;
        if (!job.IsValid())
            return false;

        JobNode& node = mPool->nodes[job.mIndex];
        {
            std::lock_guard lock(node.completionMutex);
            if (node.status.load(std::memory_order_acquire) != JobStatus::Queued)
                return true;
            node.status.store(JobStatus::Running, std::memory_order_release);
        }

        JobSystem* previousSystem = std::exchange(gExecutingJobSystem, this);
        ZoneScoped;
        ZoneName(node.name.data(), node.name.size());
        JobContext context(workerId, &node.cancellation);
        node.callable->Execute(context);
        Finish(job, node.cancellation.load(std::memory_order_acquire) ? JobStatus::Cancelled : JobStatus::Completed);
        gExecutingJobSystem = previousSystem;
        return true;
    }

    void JobSystem::WorkerMain(size_t workerId)
    {
        TracyCSetThreadName(fmt::format("{}@{}", mName.c_str(), workerId).c_str());
        while (!mStopping.load(std::memory_order_acquire))
        {
            size_t const epoch = mWakeEpoch.load(std::memory_order_acquire);
            if (TryExecuteOne(workerId))
                continue;
            if (!mStopping.load(std::memory_order_acquire))
                mWakeEpoch.wait(epoch, std::memory_order_relaxed);
        }
    }

    JobBarrier JobSystem::CreateBarrier()
    {
        bool const submitting = BeginSubmit();
        CHECK_MSG(submitting, "JobSystem shutting down");
        if (!submitting)
            return {};
        size_t const previous = mPool->liveBarriers.fetch_add(1, std::memory_order_acq_rel);
        if (previous >= mMaxBarriers)
        {
            CHECK_MSG(false, "JobSystem barrier capacity exhausted");
            mPool->liveBarriers.fetch_sub(1, std::memory_order_release);
            EndSubmit();
            return {};
        }
        JobBarrier barrier(mPool, ConstructShared<JobBarrierState>(mAllocator, mAllocator));
        EndSubmit();
        return barrier;
    }

    void JobSystem::ReleaseBarrier() noexcept
    {
        mPool->liveBarriers.fetch_sub(1, std::memory_order_release);
        mPool->liveBarriers.notify_all();
    }

    void JobSystem::Wait(JobBarrier& barrier)
    {
        CHECK_MSG(barrier.mPool.get() == mPool.get() && barrier.mState,
                  "Job barrier belongs to another JobSystem");
        CHECK_MSG(gExecutingJobSystem != this, "Jobs cannot wait on their JobSystem");
        std::unique_lock callerLock(mCallerMutex);
        {
            std::lock_guard barrierLock(barrier.mState->mutex);
            CHECK_MSG(!barrier.mState->waiting, "Job barrier already has a waiter");
            barrier.mState->waiting = true;
        }

        while (barrier.mState->pending.load(std::memory_order_acquire) != 0)
        {
            if (TryExecuteOne(mThreads.size()))
                continue;
            size_t const pending = barrier.mState->pending.load(std::memory_order_acquire);
            if (pending != 0)
                barrier.mState->pending.wait(pending, std::memory_order_relaxed);
        }

        {
            std::lock_guard lock(barrier.mState->mutex);
            barrier.mState->waiting = false;
            barrier.mState->jobs.clear();
        }
    }

    void JobSystem::CancelInternal(JobHandle const& job)
    {
        if (!job.IsValid() || job.mPool.get() != mPool.get())
            return;
        JobNode& node = mPool->nodes[job.mIndex];
        node.cancellation.store(true, std::memory_order_release);
        Finish(job, JobStatus::Cancelled);
    }

    void JobSystem::Cancel(JobHandle const& job)
    {
        CHECK_MSG(job.IsValid() && job.mPool.get() == mPool.get(), "Invalid or foreign job cancellation");
        CancelInternal(job);
    }

    void JobSystem::Join()
    {
        CHECK_MSG(gExecutingJobSystem != this, "Jobs cannot drain their JobSystem");
        std::unique_lock callerLock(mCallerMutex);
        while (mOutstanding.load(std::memory_order_acquire) != 0)
        {
            if (TryExecuteOne(mThreads.size()))
                continue;
            size_t const outstanding = mOutstanding.load(std::memory_order_acquire);
            if (outstanding != 0)
                mOutstanding.wait(outstanding, std::memory_order_relaxed);
        }
    }

    void JobSystem::Shutdown()
    {
        CHECK_MSG(gExecutingJobSystem != this, "JobSystem cannot be shut down from one of its jobs");

        {
            std::unique_lock submitLock(mSubmitMutex);
            if (!mAccepting.load(std::memory_order_relaxed))
            {
                submitLock.unlock();
                bool stopped = mStopped.load(std::memory_order_acquire);
                while (!stopped)
                {
                    mStopped.wait(false, std::memory_order_relaxed);
                    stopped = mStopped.load(std::memory_order_acquire);
                }
                return;
            }
            mAccepting.store(false, std::memory_order_release);
            mSubmitCV.wait(submitLock, [this] { return mSubmitting == 0; });
        }

        for (uint32_t i = 0; i < mPool->nodes.size(); ++i)
            if (JobHandle job = mPool->AcquireActive(mPool, i); job.IsValid())
                CancelInternal(job);

        {
            std::unique_lock callerLock(mCallerMutex);
            while (mOutstanding.load(std::memory_order_acquire) != 0)
            {
                if (TryExecuteOne(mThreads.size()))
                    continue;
                size_t const outstanding = mOutstanding.load(std::memory_order_acquire);
                if (outstanding != 0)
                    mOutstanding.wait(outstanding, std::memory_order_relaxed);
            }
        }

        mStopping.store(true, std::memory_order_release);
        mWakeEpoch.fetch_add(1, std::memory_order_release);
        mWakeEpoch.notify_all();
        for (Thread& thread : mThreads)
            if (thread.joinable())
                thread.join();

        mPool->Detach();
        mStopped.store(true, std::memory_order_release);
        mStopped.notify_all();
    }
} // namespace Foundation::Core
