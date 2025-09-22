#include <Core/DefaultAllocator.hpp>
#include <Async/ThreadPool.hpp>
using namespace Foundation;
using namespace Async;

int main()
{
    DefaultAllocator alloc;
    ThreadPool pool(4, 16, &alloc);
    Mutex printMutex;
    for (int i = 0; i < 16; ++i)
        pool.Push([&printMutex, i](){
            {
                std::scoped_lock lock(printMutex);
                printf("Work %d..\n", i);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        });
    printf("wait...\n");
    pool.Join();
    printf("work complete\n");
}