#pragma once
#include <Allocator/Allocator.hpp>
#include <string>

namespace Foundation {
	namespace Core {
		class DefaultAllocator : public Allocator {
		public:
			DefaultAllocator() = default;
			~DefaultAllocator() {
				if (m_count.load() > 0)
					BugCheck(s_BugCheckMemoryLeak);
			}
			inline pointer allocate(size_type size) override {
				m_size += size, m_count++;
				return malloc(size);
			};
			inline pointer allocate(size_type size, size_t alignment) override {
				m_size += size, m_count++;
				return malloc(size); // DefaultAllocator does not support alignment, just use malloc
			};
			inline void deallocate(pointer ptr) override {
				free(ptr);
				m_count--;
			}
			inline void deallocate(pointer ptr, size_type size) override {
				free(ptr);
				m_count--;
			}
			// Returns the total size of memory allocated by malloc
			// Deallocation is not accounted for.
			inline size_t get_allocated_size() {
				return m_size;
			}
			// Returns the total number of unreleased allocations made by malloc
			inline size_t get_allocated_count()  {
				return m_count;
			}
		private:
			std::atomic<size_t> m_size = 0; // size of the memory allocated by malloc
			std::atomic<size_t> m_count = 0; // number of allocations made by malloc
		};
	}
}