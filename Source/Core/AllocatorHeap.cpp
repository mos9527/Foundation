#if !(FOUNDATION_OS_ALLOCATOR)
#include <mimalloc.h>
#else
#include <cstdlib>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
#ifndef aligned_alloc
void* aligned_alloc(size_t alignment, size_t size) { return _aligned_malloc(size, alignment); }
void aligned_free(void* ptr) { _aligned_free(ptr); }
void* aligned_realloc(void* ptr, size_t size, size_t alignment) { return _aligned_realloc(ptr, size, alignment); }
#endif 
#endif
#include <tracy/Tracy.hpp>

namespace Foundation::Core {
    namespace
    {
        constexpr int kTracyAllocatorCallstackDepth = 16;
        constexpr const char* kTracyAllocatorName = "AllocatorHeap";

#if FOUNDATION_OS_ALLOCATOR
        size_t AllocationSize(pointer ptr)
        {
            return ptr ? _msize(ptr) : 0;
        }
#else
        size_t AllocationSize(pointer ptr)
        {
            return ptr ? mi_usable_size(ptr) : 0;
        }
#endif
    }

    pointer AllocatorHeap::Allocate(size_type size, size_t alignment) noexcept
    {
#if FOUNDATION_OS_ALLOCATOR     
        auto* ptr = aligned_alloc(alignment, size);
#else
        auto* ptr = mi_malloc_aligned(size, alignment);
#endif
        if (ptr)
        {
            mHeapUsage.fetch_add(AllocationSize(ptr), std::memory_order_relaxed);
            TracyAllocNS(ptr, size, kTracyAllocatorCallstackDepth, kTracyAllocatorName);
        }
        return ptr;
    }
    pointer AllocatorHeap::Reallocate(pointer ptr, size_type new_size, size_t alignment) noexcept
    {
        if (!ptr)
            return Allocate(new_size, alignment);
        if (new_size == 0)
        {
            Deallocate(ptr);
            return nullptr;
        }
        size_t oldSize = AllocationSize(ptr);
#if FOUNDATION_OS_ALLOCATOR
        auto* newPtr = aligned_realloc(ptr, new_size, alignment);
#else
        auto* newPtr = mi_realloc_aligned(ptr, new_size, alignment);
#endif
        if (newPtr)
        {
            size_t newSize = AllocationSize(newPtr);
            if (newSize >= oldSize)
                mHeapUsage.fetch_add(newSize - oldSize, std::memory_order_relaxed);
            else
                mHeapUsage.fetch_sub(oldSize - newSize, std::memory_order_relaxed);
            if (ptr)
                TracyFreeNS(ptr, kTracyAllocatorCallstackDepth, kTracyAllocatorName);
            TracyAllocNS(newPtr, new_size, kTracyAllocatorCallstackDepth, kTracyAllocatorName);
        }
        return newPtr;
    }
    void AllocatorHeap::QueryBudget(size_t& used, size_t& budget) const
    {
#if FOUNDATION_OS_ALLOCATOR
        used = budget = 0;
#else
        size_t elapsed_msecs,  user_msecs,  system_msecs,
                                     current_rss,  peak_rss,
                                     current_commit,  peak_commit,  page_faults;
        mi_process_info(&elapsed_msecs, &user_msecs, &system_msecs, &current_rss, &peak_rss,
                        &current_commit, &peak_commit, &page_faults);
        used = current_rss;
        budget = SIZE_MAX; // No budget info available
#endif
    }
    size_t AllocatorHeap::QueryHeapUsage() const
    {
        return mHeapUsage.load(std::memory_order_relaxed);
    }
    void AllocatorHeap::Deallocate(pointer ptr) noexcept
    {
        if (ptr)
        {
            mHeapUsage.fetch_sub(AllocationSize(ptr), std::memory_order_relaxed);
            TracyFreeNS(ptr, kTracyAllocatorCallstackDepth, kTracyAllocatorName);
        }
#if FOUNDATION_OS_ALLOCATOR
        return aligned_free(ptr);
#else
        mi_free(ptr);
#endif
    }
    Allocator* GetGlobalAllocator()
    {
        static AllocatorHeap GlobalAllocatorHeap;
        return &GlobalAllocatorHeap;
    }
}
