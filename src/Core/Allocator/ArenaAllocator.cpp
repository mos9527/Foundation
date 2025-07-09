#include <Allocator/Allocator.hpp>
#include <Allocator/ArenaAllocator.hpp>

namespace Foundation {
	namespace Core {						
		ArenaAllocator::pointer ArenaAllocator::allocate(ArenaAllocator::size_type size, ArenaAllocator::size_type alignment) {
			if (size == 0) throw std::bad_alloc();

			pointer start = AlignUp(m_current, alignment);
			pointer end = reinterpret_cast<pointer>(reinterpret_cast<char*>(start) + size);
			end = AlignUp(end, alignment);											

			if (end > m_end) throw std::bad_alloc();

			m_current = end;
			return start;
		};
		ArenaAllocator::pointer ArenaAllocator::allocate(ArenaAllocator::size_type size) {
			return allocate(size, m_default_alignment);
		};	
	}
}