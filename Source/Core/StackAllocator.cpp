#include "Allocator.hpp"
#include "StackAllocator.hpp"

namespace Foundation::Core {
pointer StackAllocator::Allocate(size_type size, size_type alignment) {
    if (size == 0) return nullptr;

    size_t start = AlignUp(m_current, alignment);
    size_t end = AlignUp(start + size, alignment);
    
	if (end > m_end) return nullptr;

	m_current = end;
    m_used += end - start;
    return reinterpret_cast<pointer>(start);
};
pointer StackAllocator::Allocate(size_type size) {
    if (size == 0) return nullptr;

    size_t start = m_current;
    size_t end = start + size;

    if (end > m_end) return nullptr;

    m_current = end;
    m_used += size;
    return reinterpret_cast<pointer>(start);
};
}