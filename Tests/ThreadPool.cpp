#include <Core/DefaultAllocator.hpp>
#include <Async/ThreadPool.hpp>
using namespace Foundation;
using namespace Async;

int main()
{
    DefaultAllocator alloc;
    ThreadPool pool(&alloc);
    Mutex printMutex;
    for (int i = 0; i < 4; ++i)
        pool.Push([&printMutex, i](){
            std::scoped_lock lock(printMutex);
            printf("Work %d..\n", i);
        });
}