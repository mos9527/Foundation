#pragma once
#include "Allocator.hpp"

namespace Foundation::Core {
	/**
     * @brief General purpose heap allocator.
     * @note By default, this allocator uses mimalloc for memory management - which can be 
     *       overridden by defining FOUNDATION_CORE_USES_OS_ALLOC to use the OS's default allocator.
	 * @note As mimalloc/OS allocators are thread-safe by default, so is this allocator.
	 */
	class AllocatorHeap : public Allocator {
	public:
        pointer Allocate(size_type size, size_t alignment) override;
        void Deallocate(pointer ptr) override;
        void Deallocate(pointer ptr, size_type size) override { Deallocate(ptr); }
        pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) override;
	};
}
