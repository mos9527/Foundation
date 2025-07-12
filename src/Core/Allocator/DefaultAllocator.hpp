#pragma once
#include <Core/Allocator/Allocator.hpp>
#include <string>

namespace Foundation {
	namespace Core {
		class DefaultAllocator : public Allocator {
		public:
			DefaultAllocator() = default;
			~DefaultAllocator() {
				if (m_count.load() > 0)
					BugCheck(sBugCheckMemoryLeak);
			}
			inline pointer Allocate(size_type size) override {
				m_size += size, m_count++;
				return malloc(size);
			};
			inline pointer Allocate(size_type size, size_t alignment) override {
				m_size += size, m_count++;
				return malloc(size); // DefaultAllocator does not support alignment, just use malloc
			};
			inline void Deallocate(pointer ptr) override {
				free(ptr);
				m_count--;
			}
			inline void Deallocate(pointer ptr, size_type size) override {
				free(ptr);
                m_count--, m_size -= size;
			}
			// Returns the total size of memory allocated by malloc
            // Deallocation with raw pointers is not tracked.
			inline size_t GetAllocatedSize() {
				return m_size;
			}
			// Returns the total number of unreleased allocations made by malloc
			inline size_t GetAllocatedCount()  {
				return m_count;
			}
		private:
			std::atomic<size_t> m_size = 0; // size of the memory allocated by malloc
			std::atomic<size_t> m_count = 0; // number of allocations made by malloc
		};
	}
}
