#pragma once
#include "Allocator.hpp"
namespace Foundation::Core {
	/**
	 * @brief Implements an atomic stack-based bump allocator.
	 * @note This implementation is thread-safe for allocations from multiple threads.
     *       Deallocation is a no-op and does not modify the internal state.
	 */
    class StackAllocator : public Allocator {
	public:
        StackAllocator() {};
        StackAllocator(Arena arena) {
            Reset(arena);
		}            
        /**
         * @brief Resets the stack allocator to the initial state, allowing for reuse of the memory block (Arena)
         */
        void Reset(Arena arena) {
            m_memory = arena.memory;
            m_current = reinterpret_cast<size_type>(arena.memory);
            m_end = reinterpret_cast<size_type>(arena.memory) + arena.size;
            m_used = 0;
        }
        /**
         * @brief Resets the stack allocator to a non-allocated state.
         */
        void Reset() {
            Reset({ nullptr, 0 });
        }
		/**
		 * @brief Allocates a block of memory of the specified size.
		 * If the requested size exceeds the available memory within the arena, returns nullptr.
		 */
        pointer Allocate(size_type size) override { return Allocate(size, sizeof(std::max_align_t)); }
		/**
		 * @brief Allocates a block of memory of the specified size with alignment.
		 * If the requested size exceeds the available memory within the arena, returns nullptr.
		 */
		pointer Allocate(size_type size, size_type alignment) override;
        /**
         * @note No-op. No memory is modified with this operation.
         */
        void Deallocate(pointer ptr) override { /* nop */ }
        /**
         * @note No-op. No memory is modified with this operation.
         */
        void Deallocate(pointer ptr, size_type size) override {  /* nop */ }
        /**
         * @note Unsupported. Throws std::runtime_error when invoked.
         */
        pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) override {
            throw std::runtime_error("StackAllocator does not support reallocation");
        }
        size_type GetUsedMemory() const noexcept override {
            return m_current - reinterpret_cast<size_type>(m_memory);
        }
        constexpr operator bool() const noexcept { return m_memory != nullptr; }
	private:		
        pointer m_memory{ nullptr };
        std::atomic<size_type> m_end{};
        std::atomic<size_type> m_current{};
        std::atomic<size_type> m_used{};
	};
}
