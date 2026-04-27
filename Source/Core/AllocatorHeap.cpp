#if !(FOUNDATION_CORE_USES_OS_ALLOC)
#include <mimalloc.h>
#else
#include <cstdlib>
#ifndef aligned_alloc
#if defined(_MSC_VER) // Thanks as always.
void* aligned_alloc(size_t alignment, size_t size) { return _aligned_malloc(size, alignment); }
void aligned_free(void* ptr) { _aligned_free(ptr); }
void* aligned_realloc(void* ptr, size_t size, size_t alignment) { return _aligned_realloc(ptr, size, alignment); }
#else
static_assert(false, "aligned_alloc not defined on this platform");
#endif
#endif 
#endif
#include <tracy/Tracy.hpp>

namespace Foundation::Core {
    namespace
    {
        constexpr int kTracyAllocatorCallstackDepth = 16;
        constexpr const char* kTracyAllocatorName = "AllocatorHeap";
    }

    pointer AllocatorHeap::Allocate(size_type size, size_t alignment) {
#if FOUNDATION_CORE_USES_OS_ALLOC     
        auto* ptr = aligned_alloc(alignment, size);
#else
        auto* ptr = mi_malloc_aligned(size, alignment);
#endif
        if (ptr)
            TracyAllocNS(ptr, size, kTracyAllocatorCallstackDepth, kTracyAllocatorName);
        return ptr;
    }
    pointer AllocatorHeap::Reallocate(pointer ptr, size_type new_size, size_t alignment) {
        if (!ptr)
            return Allocate(new_size, alignment);
        if (new_size == 0)
        {
            Deallocate(ptr);
            return nullptr;
        }
#if FOUNDATION_CORE_USES_OS_ALLOC
        auto* newPtr = aligned_realloc(ptr, new_size, alignment);
#else
        auto* newPtr = mi_realloc_aligned(ptr, new_size, alignment);
#endif
        if (newPtr)
        {
            if (ptr)
                TracyFreeNS(ptr, kTracyAllocatorCallstackDepth, kTracyAllocatorName);
            TracyAllocNS(newPtr, new_size, kTracyAllocatorCallstackDepth, kTracyAllocatorName);
        }
        return newPtr;
    }
    void AllocatorHeap::QueryBudget(size_t& used, size_t& budget) const
    {
#if FOUNDATION_CORE_USES_OS_ALLOC
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
    void AllocatorHeap::Deallocate(pointer ptr) {
        if (ptr)
            TracyFreeNS(ptr, kTracyAllocatorCallstackDepth, kTracyAllocatorName);
#if FOUNDATION_CORE_USES_OS_ALLOC
        return aligned_free(ptr);
#else
        mi_free(ptr);
#endif
    }
    Allocator* getGlobalAllocator()
    {
        static AllocatorHeap GlobalAllocatorHeap;
        return &GlobalAllocatorHeap;
    }
}
