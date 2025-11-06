#include <Async/Future.hpp>
#include <Async/Thread.hpp>
#include <Atomics/AtomicQueue.hpp>
#include <Core/AllocatorDefault.hpp>

using namespace Foundation;
using namespace Atomics;
using namespace Async;
constexpr size_t kSize = 4;
int main()
{
    DefaultAllocator alloc;
    MPMCQueue<UniquePtr<int>> queue(kSize, &alloc);
    Mutex printMutex;
    std::atomic<size_t> sum_all = 0;
    auto consume = [&](size_t index)
    {
        size_t sum = 0;
        auto reader = queue.CreateReader();
        while (true)
        {
            UniquePtr<int> value;
            if (reader.Pop(value))
            {
                std::scoped_lock lock(printMutex);
                printf("Ptr: %ld from Queue %ld\n", reinterpret_cast<size_t>(value.get()), index);
                sum += *value;
            }
            else
            {
                std::scoped_lock lock(printMutex);
                printf("Queue %ld exit. Sum=%ld\n", index, sum);
                sum_all += sum;
                return;
            }
        }
    };
    Vector<Thread> threads(&alloc);
    for (size_t i = 0; i < 32; i++)
        threads.emplace_back(consume, i);
    size_t sum_expect = 0;
    auto writer = queue.CreateWriter();
    for (size_t i = 0; i < kSize; i++)
    {
        CHECK(writer.Push(ConstructUnique<int>(&alloc, i)));
        sum_expect += i;
    }
    for (auto& t : threads)
        t.join();
    CHECK(sum_expect == sum_all);
    printf("pass!\n");
}