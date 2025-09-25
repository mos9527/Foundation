#include "HeapAllocator.hpp"

namespace Foundation::Core {
pointer HeapAllocator::Allocate(size_type size) {
    void* p = mi_malloc(size);
    mUsed += mi_usable_size(p);
    return p;
}
pointer HeapAllocator::Allocate(size_type size, size_t alignment) {
    void* p = mi_malloc_aligned(size, alignment);
    mUsed += mi_usable_size(p);
    return p;
}
pointer HeapAllocator::Reallocate(pointer ptr, size_type new_size, size_t alignment) {
    size_t old_size = 0;
    old_size = mi_usable_size(ptr);
    void* p = mi_realloc_aligned(ptr, new_size, alignment);
    mUsed -= old_size, mUsed += mi_usable_size(p);
    return p;
}
void HeapAllocator::Deallocate(pointer ptr) {
    size_t size = mi_usable_size(ptr);
    mUsed -= size;
    mi_free(ptr);
}
}