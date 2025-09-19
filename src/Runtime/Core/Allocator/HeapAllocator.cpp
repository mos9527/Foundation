#include "HeapAllocator.hpp"

namespace Foundation::Core {
template<bool T>
pointer HeapAllocator<T>::Allocate(size_type size) {
    void* p = mi_malloc(size);
    if constexpr (T)
    {
        m_used += mi_usable_size(p);
    }
    return p;
}
template<bool T>
pointer HeapAllocator<T>::Allocate(size_type size, size_t alignment) {
    void* p = mi_malloc_aligned(size, alignment);
    if constexpr (T)
    {
        m_used += mi_usable_size(p);
    }
    return p;
}
template<bool T>
pointer HeapAllocator<T>::Reallocate(pointer ptr, size_type new_size, size_t alignment) {
    size_t old_size = 0;
    if constexpr (T)
        old_size = mi_usable_size(ptr);
    void* p = mi_realloc_aligned(ptr, new_size, alignment);
    if constexpr (T)
    {
        m_used -= old_size, m_used += mi_usable_size(p);
    }
    return p;
}
template<bool T>
void HeapAllocator<T>::Deallocate(pointer ptr) {
    if constexpr (T)
    {
        size_t size = mi_usable_size(ptr);
        m_used -= size;
    }
    mi_free(ptr);
}
template class HeapAllocator<true>;
template class HeapAllocator<false>;
}