#include "JobGraph.hpp"
namespace Foundation::Core
{
    // One pool job per Worker node: run the body once, then settle the node.
    struct JobGraph::WorkerJob final : Job
    {
        JobGraph* graph;
        size_t id;
        WorkerJob(JobGraph* graph, size_t id) : graph(graph), id(id) {}
        void Execute(size_t workerId) noexcept override
        {
            Node& node = *graph->mNodes[id];
            if (node.work)
                node.work->RunSingle(workerId);
            graph->OnNodeFinished(id);
        }
    };

    // One of N pool jobs fanned out for a ParallelFor node: drain the shared cursor; the last worker
    // to leave (latch hits zero) settles the node. Mirrors ThreadPool::ParallelForAsync.
    struct JobGraph::ParallelForJob final : Job
    {
        JobGraph* graph;
        size_t id;
        ParallelForJob(JobGraph* graph, size_t id) : graph(graph), id(id) {}
        void Execute(size_t workerId) noexcept override
        {
            Node& node = *graph->mNodes[id];
            for (size_t i; (i = node.cursor.fetch_add(1, std::memory_order_relaxed)) < node.total;)
                node.work->RunIndex(i, workerId);
            if (node.remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
                graph->OnNodeFinished(id);
        }
    };

    JobGraph::JobGraph(ThreadPool& pool, Allocator* allocator) :
        mPool(pool), mAllocator(allocator), mNodes(allocator), mMainReady(Deque<size_t>(allocator))
    {
    }

    JobGraph::~JobGraph()
    {
        // The graph backs every in-flight pool job, so it must outlive them. Drain defensively in
        // case a caller forgot to Wait().
        if (mSubmitted)
            Wait();
    }

    size_t JobGraph::AddNode(NodeKind kind, StringView name, UniquePtr<IJobWork> work, size_t total,
                             ExecutionPolicy policy)
    {
        CHECK_MSG(!mSubmitted, "JobGraph: cannot add nodes after Submit()");
        auto node = ConstructUnique<Node>(mAllocator, mAllocator);
        node->kind = kind;
        node->name = name;
        node->work = std::move(work);
        node->total = total;
        node->policy = policy;
        mNodes.push_back(std::move(node));
        return mNodes.size() - 1;
    }

    void JobGraph::AddEdge(size_t producer, size_t consumer)
    {
        CHECK_MSG(!mSubmitted, "JobGraph: cannot add edges after Submit()");
        CHECK(producer < mNodes.size() && consumer < mNodes.size());
        mNodes[producer]->dependents.push_back(consumer);
        mNodes[consumer]->pending++;
    }

    JobHandle JobGraph::AddBarrier(StringView name)
    {
        return {AddNode(NodeKind::Barrier, name, UniquePtr<IJobWork>{nullptr, StlDeleter<IJobWork>{nullptr}}, 0,
                        ExecutionPolicy::Par)};
    }

    void JobGraph::Schedule(size_t id)
    {
        Node& node = *mNodes[id];
        if (node.kind == NodeKind::Barrier)
        {
            OnNodeFinished(id); // no body to run
            return;
        }
        // Main nodes, anything forced serial, and a worker-less pool all run on the Wait/Pump thread.
        bool const toMain = node.kind == NodeKind::Main || node.policy == ExecutionPolicy::Seq ||
                            mPool.GetWorkerCount() == 0;
        if (toMain)
            EnqueueMain(id);
        else if (node.kind == NodeKind::ParallelFor)
            DispatchParallelFor(id);
        else
            DispatchWorker(id);
    }

    void JobGraph::DispatchWorker(size_t id) { mPool.PushImplAlloc<WorkerJob>(mAllocator, this, id); }

    void JobGraph::DispatchParallelFor(size_t id)
    {
        Node& node = *mNodes[id];
        if (node.total == 0)
        {
            OnNodeFinished(id);
            return;
        }
        size_t const helpers = std::min(mPool.GetWorkerCount(), node.total);
        // Arm the latch before pushing: a worker may run (and settle the node) before this returns.
        node.cursor.store(0, std::memory_order_relaxed);
        node.remaining.store(helpers, std::memory_order_relaxed);
        for (size_t i = 0; i < helpers; ++i)
            mPool.PushImplAlloc<ParallelForJob>(mAllocator, this, id);
    }

    void JobGraph::EnqueueMain(size_t id)
    {
        {
            std::lock_guard lock(mMutex);
            mMainReady.push(id);
        }
        mCond.notify_all();
    }

    void JobGraph::RunMainNode(size_t id)
    {
        Node& node = *mNodes[id];
        if (node.kind == NodeKind::ParallelFor)
        {
            for (size_t i = 0; i < node.total; ++i)
                node.work->RunIndex(i, mMainWorkerId);
        }
        else if (node.work)
        {
            node.work->RunSingle(mMainWorkerId);
        }
        OnNodeFinished(id);
    }

    void JobGraph::OnNodeFinished(size_t id)
    {
        // Settle the node and collect the dependents it just unblocked, all under the lock; dispatch
        // them after releasing it (dispatch may push to the pool or recurse through barriers).
        Vector<size_t> ready(mAllocator);
        {
            std::lock_guard lock(mMutex);
            Node& node = *mNodes[id];
            node.finished = true;
            for (size_t dep : node.dependents)
            {
                Node& consumer = *mNodes[dep];
                CHECK_MSG(consumer.pending > 0, "JobGraph: dependency count underflow");
                if (--consumer.pending == 0)
                    ready.push_back(dep);
            }
            if (mNodesRemaining > 0)
                --mNodesRemaining;
        }
        mCond.notify_all(); // a Wait() target may now be done, or the graph fully drained
        for (size_t dep : ready)
            Schedule(dep);
    }

    void JobGraph::Submit()
    {
        CHECK_MSG(!mSubmitted, "JobGraph: Submit() called twice");
        mMainWorkerId = mPool.GetWorkerCount();
        Vector<size_t> ready(mAllocator);
        {
            std::lock_guard lock(mMutex);
            mSubmitted = true;
            mNodesRemaining = mNodes.size();
            for (size_t i = 0; i < mNodes.size(); ++i)
                if (mNodes[i]->pending == 0)
                    ready.push_back(i);
        }
        for (size_t id : ready)
            Schedule(id);
    }

    void JobGraph::PumpMainThread()
    {
        for (;;)
        {
            size_t id;
            {
                std::lock_guard lock(mMutex);
                if (mMainReady.empty())
                    return;
                id = mMainReady.front();
                mMainReady.pop();
            }
            RunMainNode(id);
        }
    }

    void JobGraph::Wait(JobHandle target)
    {
        CHECK_MSG(mSubmitted, "JobGraph: Wait() before Submit()");
        std::unique_lock lock(mMutex);
        for (;;)
        {
            // Drain ready main-thread nodes on this thread first (release the lock while running).
            while (!mMainReady.empty())
            {
                size_t const id = mMainReady.front();
                mMainReady.pop();
                lock.unlock();
                RunMainNode(id);
                lock.lock();
            }
            bool const done = target.Valid() ? mNodes[target.id]->finished : mNodesRemaining == 0;
            if (done || mNodesRemaining == 0)
                break;
            mCond.wait(lock);
        }
    }
} // namespace Foundation::Core
