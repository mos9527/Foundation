#pragma once
#include "ThreadPool.hpp"
#include <functional>

namespace Foundation::Core
{
    enum class JobStatus : uint8_t
    {
        Waiting,
        Queued,
        Running,
        Completed,
        Failed,
        Cancelled,
    };

    struct JobSystemDesc
    {
        size_t workerCount{};
        size_t maxJobs{1024};
        size_t maxBarriers{16};
        size_t readyQueueSize{1024};
        Allocator* allocator{};
        StringView name{"Job"};
    };

    class JobSystem;
    class JobHandle;
    class JobDependency;
    struct JobPool;
    struct JobBarrierState;

    class JobContext
    {
        friend class JobSystem;
        size_t mWorkerId{};
        Atomic<bool> const* mCancellation{};

        JobContext(size_t workerId, Atomic<bool> const* cancellation) :
            mWorkerId(workerId), mCancellation(cancellation)
        {
        }

    public:
        [[nodiscard]] size_t GetWorkerId() const noexcept { return mWorkerId; }
        [[nodiscard]] bool IsCancellationRequested() const noexcept
        {
            return mCancellation && mCancellation->load(std::memory_order_acquire);
        }
    };

    class JobHandle
    {
        friend class JobSystem;
        friend class JobBarrier;
        friend class JobDependency;
        friend struct JobPool;
        SharedPtr<JobPool> mPool;
        uint32_t mIndex{UINT32_MAX};
        uint32_t mGeneration{};

        struct AdoptRef
        {
        };
        JobHandle(SharedPtr<JobPool> pool, uint32_t index, uint32_t generation, AdoptRef) noexcept;
        void AddRef() const noexcept;
        void Release() noexcept;

    public:
        JobHandle() = default;
        JobHandle(JobHandle const& other) noexcept;
        JobHandle& operator=(JobHandle const& other) noexcept;
        JobHandle(JobHandle&& other) noexcept;
        JobHandle& operator=(JobHandle&& other) noexcept;
        ~JobHandle();

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool IsDone() const noexcept;
        [[nodiscard]] JobStatus Status() const noexcept;
        void AddDependency(uint32_t count = 1) const;
        void RemoveDependency(uint32_t count = 1) const;
        [[nodiscard]] JobDependency AddDependencyGuard(uint32_t count = 1) const;
        [[nodiscard]] JobDependency AdoptDependencyGuard(uint32_t count = 1) const;
        static void RemoveDependencies(Span<const JobHandle> jobs, uint32_t count = 1);
    };

    class JobDependency
    {
        JobHandle mJob;
        uint32_t mCount{};

        friend class JobHandle;
        JobDependency(JobHandle job, uint32_t count, bool add);

    public:
        JobDependency() = default;
        JobDependency(JobDependency const&) = delete;
        JobDependency& operator=(JobDependency const&) = delete;
        JobDependency(JobDependency&& other) noexcept;
        JobDependency& operator=(JobDependency&& other) noexcept;
        ~JobDependency();

        void Release();
    };

    class JobBarrier
    {
        friend class JobSystem;
        SharedPtr<JobPool> mPool;
        SharedPtr<JobBarrierState> mState;

        JobBarrier(SharedPtr<JobPool> pool, SharedPtr<JobBarrierState> state) :
            mPool(std::move(pool)), mState(std::move(state))
        {
        }
        void Reset() noexcept;

    public:
        JobBarrier() = default;
        JobBarrier(JobBarrier const&) = delete;
        JobBarrier& operator=(JobBarrier const&) = delete;
        JobBarrier(JobBarrier&& other) noexcept;
        JobBarrier& operator=(JobBarrier&& other) noexcept;
        ~JobBarrier();

        void Add(JobHandle const& job);
        void Add(Span<const JobHandle> jobs);
        [[nodiscard]] bool IsEmpty() const noexcept;
    };

    class JobCallable
    {
    public:
        virtual ~JobCallable() = default;
        virtual void Execute(JobContext& context) = 0;
    };

    template <typename Fn>
    class LambdaJobCallable final : public JobCallable
    {
        Fn mFunction;

    public:
        explicit LambdaJobCallable(Fn&& function) : mFunction(std::move(function)) {}

        void Execute(JobContext& context) override
        {
            if constexpr (std::is_invocable_v<Fn&, JobContext&>)
                std::invoke(mFunction, context);
            else
                std::invoke(mFunction);
        }
    };

    class JobSystem
    {
        Allocator* mAllocator;
        String mName;
        size_t mMaxBarriers;
        SharedPtr<JobPool> mPool;

        Atomic<bool> mAccepting{true};
        Mutex mSubmitMutex;
        CondVar mSubmitCV;
        size_t mSubmitting{};
        Atomic<bool> mStopping{false};
        Atomic<bool> mStopped{false};
        Atomic<size_t> mWakeEpoch{0};
        Atomic<size_t> mOutstanding{0};
        Mutex mCallerMutex;

        using ReadyQueue = MPMCQueue<JobHandle>;
        using ReadyQueues = Array<ReadyQueue, kJobPriorityCount>;
        ReadyQueues mReady;
        Vector<Thread> mThreads;

        static constexpr size_t PriorityIndex(JobPriority priority) noexcept
        {
            return static_cast<size_t>(priority);
        }

        bool BeginSubmit() noexcept;
        void EndSubmit() noexcept;
        JobHandle CreateJobInternal(StringView name, JobPriority priority, UniquePtr<JobCallable> callable,
                                    uint32_t dependencyCount);
        void Queue(JobHandle const& job) noexcept;
        void ReleaseDependency(JobHandle const& job, uint32_t count, bool prerequisiteFailed);
        void Finish(JobHandle const& job, JobStatus status) noexcept;
        void RegisterSuccessor(JobHandle const& prerequisite, JobHandle const& dependent);
        bool TryExecuteOne(size_t workerId);
        void WorkerMain(size_t workerId);
        void ReleaseBarrier() noexcept;
        void CancelInternal(JobHandle const& job);

        friend class JobHandle;
        friend class JobBarrier;
        friend class JobDependency;
    public:
        explicit JobSystem(JobSystemDesc const& desc);
        JobSystem(JobSystem const&) = delete;
        JobSystem& operator=(JobSystem const&) = delete;
        ~JobSystem();

        [[nodiscard]] size_t GetWorkerCount() const noexcept { return mThreads.size(); }
        [[nodiscard]] size_t GetMaxConcurrency() const noexcept { return mThreads.size() + 1; }

        template <typename Fn>
        JobHandle CreateJob(StringView name, JobPriority priority, Fn&& function, uint32_t dependencyCount = 0)
        {
            bool const submitting = BeginSubmit();
            CHECK_MSG(submitting, "JobSystem shutting down");
            if (!submitting)
                return {};
            using Function = std::decay_t<Fn>;
            auto callable = ConstructUniqueBase<JobCallable, LambdaJobCallable<Function>>(
                mAllocator, Function(std::forward<Fn>(function)));
            JobHandle job = CreateJobInternal(name, priority, std::move(callable), dependencyCount);
            EndSubmit();
            return job;
        }

        template <typename Fn>
        JobHandle CreateJob(StringView name, Fn&& function, uint32_t dependencyCount = 0)
        {
            return CreateJob(name, JobPriority::Normal, std::forward<Fn>(function), dependencyCount);
        }

        template <typename Fn>
        JobHandle CreateJobAfter(StringView name, JobPriority priority, Span<const JobHandle> prerequisites,
                                 Fn&& function)
        {
            bool const submitting = BeginSubmit();
            CHECK_MSG(submitting, "JobSystem shutting down");
            if (!submitting)
                return {};
            for (JobHandle const& prerequisite : prerequisites)
                CHECK_MSG(prerequisite.IsValid() && prerequisite.mPool.get() == mPool.get(),
                          "Invalid or foreign job prerequisite");

            using Function = std::decay_t<Fn>;
            auto callable = ConstructUniqueBase<JobCallable, LambdaJobCallable<Function>>(
                mAllocator, Function(std::forward<Fn>(function)));
            JobHandle dependent = CreateJobInternal(
                name, priority, std::move(callable), static_cast<uint32_t>(prerequisites.size() + 1));
            for (JobHandle const& prerequisite : prerequisites)
                RegisterSuccessor(prerequisite, dependent);
            dependent.RemoveDependency();
            EndSubmit();
            return dependent;
        }

        template <typename Fn>
        JobHandle CreateJobAfter(StringView name, Span<const JobHandle> prerequisites, Fn&& function)
        {
            return CreateJobAfter(name, JobPriority::Normal, prerequisites, std::forward<Fn>(function));
        }

        JobBarrier CreateBarrier();
        void Wait(JobBarrier& barrier);
        void Wait(JobBarrier&& barrier) { Wait(barrier); }
        void Cancel(JobHandle const& job);
        void Drain();
        void Shutdown();

        template <typename Fn>
        JobHandle Dispatch(StringView name, size_t count, size_t step, JobPriority priority, Fn&& function)
        {
            CHECK_MSG(step != 0, "Job dispatch step size must be non-zero");
            if (step == 0)
                return {};
            if (count == 0)
                return CreateJob(name, priority, [] {});

            using Function = std::decay_t<Fn>;
            auto sharedFunction = ConstructShared<Function>(mAllocator, std::forward<Fn>(function));
            size_t const chunkCount = 1 + (count - 1) / step;
            Vector<JobHandle> chunks(mAllocator);
            chunks.reserve(chunkCount);
            for (size_t begin = 0; begin < count; begin += step)
            {
                size_t const end = std::min(begin + step, count);
                chunks.push_back(CreateJob(
                    name, priority,
                    [sharedFunction, begin, end](JobContext& context)
                    {
                        std::invoke(*sharedFunction, begin, end, context);
                    }));
            }
            return CreateJobAfter(name, priority, Span<const JobHandle>{chunks.data(), chunks.size()}, [] {});
        }

        template <typename Fn>
        JobBarrier ParallelFor(ExecutionPolicy policy, StringView name, size_t count, size_t step, Fn&& function)
        {
            CHECK_MSG(step != 0, "ParallelFor grain size must be non-zero");
            if (step == 0)
                return CreateBarrier();
            if (count == 0)
                return CreateBarrier();
            if (policy == ExecutionPolicy::Seq || count <= step || mThreads.empty())
            {
                JobContext context(mThreads.size(), nullptr);
                std::invoke(function, size_t{0}, count, context);
                return CreateBarrier();
            }
            JobBarrier barrier = CreateBarrier();
            JobHandle completion =
                Dispatch(name, count, step, JobPriority::Normal, std::forward<Fn>(function));
            barrier.Add(completion);
            return barrier;
        }
    };
} // namespace Foundation::Core
