#pragma once
#include <Core/Allocator/Allocator.hpp>

namespace Foundation {
	namespace Core {
		class ArenaAllocator : public Allocator {
		public:
			ArenaAllocator(pointer memory, size_type size, size_type default_alignment = alignof(std::max_align_t)) : m_memory(memory), m_current(memory), m_default_alignment(default_alignment) {
				m_end = reinterpret_cast<pointer>(reinterpret_cast<char*>(memory) + size);
			}

			pointer Allocate(size_type size) override;
			pointer Allocate(size_type size, size_type alignment) override;

			inline void Deallocate(pointer ptr) { /* nop */ }
			inline void Deallocate(pointer ptr, size_type size) { /* nop */ }

			inline size_t GetAllocatedSize() {
				return reinterpret_cast<size_t>(m_current.load()) - reinterpret_cast<size_t>(m_memory);
			}
			inline size_t GetFreeSize() {
				return reinterpret_cast<size_t>(m_end) - reinterpret_cast<size_t>(m_current.load());
			}
		private:		
			const size_type m_default_alignment; // default alignment for allocations
			const pointer m_memory; // pointer to the start of the arena memory
			pointer m_end; // pointer to the end of the arena memory
			std::atomic<pointer> m_current; // top of the arena memory
		};
	} // namespace Core::Allocator
}
