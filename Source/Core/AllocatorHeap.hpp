#pragma once
#include "Allocator.hpp"

namespace Foundation::Core {
	/**
     * @brief General purpose heap allocator.
     * @note By default, this allocator uses mimalloc for memory management - which can be 
     *       overridden by defining FOUNDATION_CORE_USES_OS_ALLOC to use the OS's default allocator.
	 * @note As mimalloc/OS allocators are thread-safe by default, so is this allocator.
	 *       This is *only* a thin wrapper around the underlying allocator, and is thus stateless.
	 * @note Queried values will reflect the global state of the underlying allocator.
	 */
	class AllocatorHeap : public Allocator {
	public:
        pointer Allocate(size_type size, size_t alignment) override;
        void Deallocate(pointer ptr) override;
        pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) override;
        /**
	     * @note Queried values will reflect the global state of the underlying allocator.
	     */
        void QueryBudget(size_t& used, size_t& budget) const override;
	};
}
