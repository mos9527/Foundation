#include "HeapAllocator.hpp"

namespace Foundation::Core {
pointer HeapAllocator::Allocate(size_type size) {
    return mi_malloc(size);
}
pointer HeapAllocator::Allocate(size_type size, size_t alignment) {
    return mi_malloc_aligned(size, alignment);
}
pointer HeapAllocator::Reallocate(pointer ptr, size_type new_size, size_t alignment) {
    return mi_realloc_aligned(ptr, new_size, alignment);
}
void HeapAllocator::Deallocate(pointer ptr) {
    mi_free(ptr);
}
}