#include <Async/ThreadPool.hpp>
#include <Core/AllocatorDefault.hpp>
using namespace Foundation;
using namespace Async;

int main()
{
    DefaultAllocator alloc;
    ThreadPool pool(16, 64, &alloc);
    auto job = []
    {
        auto id =std::hash<std::thread::id>()(std::this_thread::get_id());
        LOG(Job, info, "Starting in {}", id);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        LOG(Job, info, "Done in {}", id);
        return id;
    };
    auto fut1 = pool.Push(job);
    auto fut2 = pool.Push(job);
    printf("wait...\n");
    pool.Join();
    LOG(fut1, info, "{}", fut1->get_future().get());
    LOG(fut2, info, "{}", fut2->get_future().get());
}