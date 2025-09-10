#include <Container/CircularBuffer.hpp>
#include <Allocator/DefaultAllocator.hpp>
using namespace Foundation::Core;
DefaultAllocator alloc;
int main()
{
    CircularBuffer<int> buffer(2, &alloc);
    auto print_content = [&buffer]()  {
        for (const auto& v : buffer)
            printf("%d,", v);
        printf("\n");
    };
    printf("insertion\n");
    for (int i = 0; i < 32; i++)
    {
        buffer.emplace_back(i);
        print_content();
    }
    printf("pop_front()\n");
    buffer.pop_front();
    print_content();
    printf("pop_back()\n");
    buffer.pop_back();
    print_content();
    printf("pop_front() until empty\n");
    while (!buffer.empty())
    {
        buffer.pop_front();
        print_content();
    }
    printf("insertion\n");
    for (int i = 0; i < 32; i++)
    {
        buffer.emplace_back(i);
        print_content();
    }
    printf("pop_back() until empty\n");
    while (!buffer.empty())
    {
        buffer.pop_back();
        print_content();
    }
}