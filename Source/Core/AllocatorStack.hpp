#pragma once
#include "Allocator.hpp"
#include "Atomic.hpp"
namespace Foundation::Core {
	/**
	 * @brief Implements an lock-free stack-based bump allocator.
	 * @note This implementation is thread-safe for allocations from multiple threads.
     *       Deallocation is a no-op and does not modify the internal state.
	 */
    class AllocatorStack : public Allocator {
	public:
        AllocatorStack() = default;
        AllocatorStack(Arena arena) {
            Reset(arena);
		}            
        /**
         * @brief Resets the stack allocator to the initial state, allowing for reuse of the memory block (Arena)
         */
        void Reset(Arena arena) {
            mMemory = arena.memory;
            mCurrent = reinterpret_cast<size_type>(arena.memory);
            mEnd = reinterpret_cast<size_type>(arena.memory) + arena.size;
        }
        /**
         * @brief Resets the stack allocator to a non-allocated state.
         */
        void Reset() {
            Reset({ nullptr, 0 });
        }
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
         * @note Unsupported. Throws std::runtime_error when invoked.
         */
        pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) override {
            throw std::runtime_error("Allocator does not support reallocation");
        }
        constexpr operator bool() const noexcept { return mMemory != nullptr; }
	private:		
        pointer mMemory{ nullptr };
        Atomic<size_type> mCurrent{};
        size_type mEnd{};
	};
}
