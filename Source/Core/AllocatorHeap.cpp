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
namespace Foundation::Core {
    pointer AllocatorHeap::Allocate(size_type size, size_t alignment) {
#if FOUNDATION_CORE_USES_OS_ALLOC     
        return aligned_alloc(alignment, size);
#else
        return mi_malloc_aligned(size, alignment);
#endif
    }
    pointer AllocatorHeap::Reallocate(pointer ptr, size_type new_size, size_t alignment) {
#if FOUNDATION_CORE_USES_OS_ALLOC
        return aligned_realloc(ptr, new_size, alignment);
#else
        return mi_realloc_aligned(ptr, new_size, alignment);
#endif
    }
    void AllocatorHeap::QueryBudget(size_t& used, size_t& budget) const
    {
        size_t elapsed_msecs,  user_msecs,  system_msecs,
                                     current_rss,  peak_rss,
                                     current_commit,  peak_commit,  page_faults;
        mi_process_info(&elapsed_msecs, &user_msecs, &system_msecs, &current_rss, &peak_rss,
                        &current_commit, &peak_commit, &page_faults);
        used = current_rss;
        budget = SIZE_MAX; // No budget info available
    }
    void AllocatorHeap::Deallocate(pointer ptr) {
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
