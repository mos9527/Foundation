#include "Allocator.hpp"
#include "StackAllocator.hpp"

namespace Foundation::Core {
    pointer StackAllocator::Allocate(size_type size, size_type alignment)
    {
        if (size == 0)
            return nullptr;        
        size_t current = m_current.load(std::memory_order_relaxed);
        while (true)
        {
            auto aligned = AlignUp(current, alignment);
            auto next = aligned + size;

            if (next > m_end) [[unlikely]]
                return nullptr;            
            
            if (m_current.compare_exchange_weak(current, next, std::memory_order_release,
                                                std::memory_order_relaxed))
            {
                // Bump OK. Good to go!
                m_used.fetch_add(size, std::memory_order_relaxed);
                return reinterpret_cast<pointer>(aligned);
            }
            current = m_current.load(std::memory_order_relaxed);
        }
    };
}
