#include <Core/DefaultAllocator.hpp>
#include <Atomic/Queue.hpp>
#include <Async/Thread.hpp>
using namespace Foundation;
using namespace Atomic;
using namespace Async;
constexpr size_t kSize = 1024;
int main()
{
    DefaultAllocator alloc;
    SPSCQueue<int> queue(kSize, &alloc);
    Thread producer([&queue]() {
        for (int i = 0; i < kSize; i++)
        {
            while (!queue.push(i));
        }
    });
    Thread consumer([&queue]()
    {
        while (true)
        {
            auto value = queue.pop();
            if (value.has_value())
            {
                printf("Got: %d\n", value.value());
            }
        }
    });
}