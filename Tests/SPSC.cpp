#include <Core/DefaultAllocator.hpp>
#include <Atomics/Queue.hpp>
#include <Async/Thread.hpp>
#include <Async/Future.hpp>
using namespace Foundation;
using namespace Atomics;
using namespace Async;
constexpr size_t kSize = 1LL << 20;
int main()
{
    DefaultAllocator alloc;
    SPSCQueue<int> queue(kSize, &alloc);
    size_t sum_expect = 0;
    Thread producer([&]() {
        for (int i = 0; i < kSize; i++)
        {
            while (!queue.Push(i));
            sum_expect += i;
        }
    });
    producer.join();
    std::atomic<size_t> sum_all = 0;
    Mutex printMutex;
    Thread consumer([&]()
    {
        int value;
        while (!queue.Pop(value));
        while (true)
        {
            sum_all += value;
            if (!queue.Pop(value))
            {
                std::scoped_lock lock(printMutex);
                printf("Got sum: %ld\n", sum_all.load());
                return;
            }
        }
    });
    consumer.join();
    CHECK(sum_expect == sum_all);
    {
        std::scoped_lock lock(printMutex);
        printf("pass!\n");
    }
}