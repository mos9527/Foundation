#include <../src/Rendering/Staging.hpp>

#include "Allocator/DefaultAllocator.hpp"
using namespace Foundation::Rendering;
DefaultAllocator allocator;
int main()
{
    BufferStagingList list(allocator.Ptr());
    auto PrintStaging = [&]()
    {
        for (auto const& [src, dst, region] : list)
            printf("Src: %p, Dst: %p, Src Off: %zu, Dst Off: %zu Size: %zu\n", src, dst, region.src_offset, region.dst_offset, region.size);
    };
    auto Add = [&](auto src, auto dst, size_t src_offset, size_t dst_offset, size_t size)
    {
        list.emplace_back(reinterpret_cast<RHIBuffer*>(src), reinterpret_cast<RHIBuffer*>(dst), RHICommandList::CopyBufferRegion{src_offset, dst_offset, size});
    };
    Add(1, 2, 0, 0, 128);
    Add(1, 2, 128, 128, 128);
    Add(3, 2, 256, 256, 128);

    printf("Before\n");
    PrintStaging();
    CoalesceBufferStaging(list);
    printf("After\n");
    PrintStaging();
}