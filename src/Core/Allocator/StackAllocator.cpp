#include <Core/Allocator/Allocator.hpp>
#include <Core/Allocator/StackAllocator.hpp>
using namespace Foundation::Core;
template<typename Counter>
StackAllocator<Counter>::pointer StackAllocator<Counter>::Allocate(StackAllocator::size_type size, StackAllocator::size_type alignment) {
    if (size == 0 || !m_memory) return nullptr;

    size_t start = AlignUp(m_current, alignment);
    size_t end = AlignUp(start + size, alignment);
    
	if (end > m_end) return nullptr;

	m_current = end;
    m_used += end - start;
    return reinterpret_cast<pointer>(start);
};
template<typename Counter>
StackAllocator<Counter>::pointer StackAllocator<Counter>::Allocate(StackAllocator::size_type size) {
    if (size == 0 || !m_memory) return nullptr;

    size_t start = m_current;
    size_t end = start + size;

    if (end > m_end) return nullptr;

    m_current = end;
    m_used += size;
    return reinterpret_cast<pointer>(start);
};

template class StackAllocator<CounterSingleThreaded>;
template class StackAllocator<CounterMultiThreaded>;
