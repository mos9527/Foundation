#pragma once
#include <Core/Allocator/Allocator.hpp>
#include <mimalloc.h>
#include <string>

namespace Foundation {
	namespace Core {
		class MimallocAllocator : public Allocator {
		public:
			MimallocAllocator() = default;
			~MimallocAllocator() {
				if (m_count.load() > 0)
					BugCheck(s_BugCheckMemoryLeak);
			}
			inline pointer Allocate(size_type size) override {
				m_size += size, m_count++;
				return mi_malloc(size);
			};
			inline pointer Allocate(size_type size, size_t alignment) override {
				m_size += size, m_count++;
				return mi_malloc_aligned(size, alignment);
			}
			inline void Deallocate(pointer ptr) override {
				mi_free(ptr);
				m_count--;
			}
			inline void Deallocate(pointer ptr, size_type size) override {
				mi_free(ptr);
				m_count--;
			}
			// Returns the total size of memory allocated by mimalloc
			// Deallocation is not accounted for.
			inline size_t GetAllocatedSize() {
				return m_size;
			}
			// Returns the total number of unreleased allocations made by mimalloc
			inline size_t GetAllocatedCount()  {
				return m_count;
			}
		private:
			std::atomic<size_t> m_size = 0; // size of the memory allocated by mimalloc
			std::atomic<size_t> m_count = 0; // number of allocations made by mimalloc
		};
	}
}
