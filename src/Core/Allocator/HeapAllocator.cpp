#include <Core/Allocator/HeapAllocator.hpp>
using namespace Foundation::Core;
template<typename Counter>
HeapAllocator<Counter>::pointer HeapAllocator<Counter>::Allocate(size_type size) {
    void* p;
    if (!m_heap || size > kHeapMaxAllocation)
        p = mi_malloc(size);
    else
        p = mi_heap_malloc(m_heap, size);
    m_used += mi_usable_size(p);
    return p;
}
template<typename Counter>
HeapAllocator<Counter>::pointer HeapAllocator<Counter>::Allocate(size_type size, size_t alignment) {
    void* p;
    if (!m_heap || size > kHeapMaxAllocation)
        p = mi_malloc_aligned(size, alignment);
    else
        p = mi_heap_malloc_aligned(m_heap, size, alignment);
    m_used += mi_usable_size(p);
    return p;
}
template<typename Counter>
HeapAllocator<Counter>::pointer HeapAllocator<Counter>::Reallocate(pointer ptr, size_type new_size, size_t alignment) {
    size_t old_size = mi_usable_size(ptr);
    void* p; 
    if (!m_heap || new_size > kHeapMaxAllocation)
         p = mi_realloc_aligned(ptr, new_size, alignment);
    else
        p = mi_heap_realloc_aligned(m_heap, ptr, new_size, alignment);
    m_used -= old_size, m_used += mi_usable_size(p);
    return p;
}
template<typename Counter>
void HeapAllocator<Counter>::Deallocate(pointer ptr) {
    size_t size = mi_usable_size(ptr);
    mi_free(ptr);
    m_used -= size;
}

template class HeapAllocator<CounterSingleThreaded>;
template class HeapAllocator<CounterMultiThreaded>;
