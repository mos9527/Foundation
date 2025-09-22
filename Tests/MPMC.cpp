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
    MPMCQueue<int> queue(kSize, &alloc);
    size_t sum_expect = 0;
    Thread producer([&]() {
        auto writer = queue.create_writer();
        for (int i = 0; i < kSize; i++)
        {
            while (!writer.push(i));
            sum_expect += i;
        }
    });
    producer.join();
    Mutex printMutex;
    std::atomic<size_t> sum_all = 0;
    auto consume = [&](size_t index)
    {
        size_t sum = 0;
        auto reader = queue.create_reader();
        while (true)
        {
            int value;
            if (reader.pop(value))
            {
                sum += value;
            } else
            {
                std::scoped_lock lock(printMutex);
                printf("Queue %ld exit. Sum=%ld\n", index, sum);
                sum_all += sum;
                return;
            }
        }
    };
    Thread c1(consume, 1), c2(consume, 2), c3(consume, 3), c4(consume, 4);
    c1.join(), c2.join(), c3.join(), c4.join();
    CHECK(sum_expect == sum_all);
    printf("pass!\n");
}