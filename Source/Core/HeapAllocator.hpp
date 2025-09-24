#pragma once
#include <mimalloc.h>
#include "Allocator.hpp"

namespace Foundation::Core {
	/**
	 * @brief Wrapper around mimalloc's heap allocation functionalities.
	 * @note As mimalloc is thread-safe by default, so is this allocator.
	 */
	class HeapAllocator : public Allocator {
        std::atomic<uint64_t> m_used{};
	public:
        pointer Allocate(size_type size) override;
        pointer Allocate(size_type size, size_t alignment) override;
        void Deallocate(pointer ptr) override;
        void Deallocate(pointer ptr, size_type size) override { Deallocate(ptr); }
        pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) override;
        /**
         * @return Used memory in bytes. If tracking is disabled, this will always return 0.
         */
        size_type GetUsedMemory() const noexcept override { return m_used.load(std::memory_order_relaxed); }
	};
}
