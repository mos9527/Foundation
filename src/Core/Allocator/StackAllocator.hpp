#pragma once
#include <Core/Allocator/Allocator.hpp>
namespace Foundation {
	namespace Core {
		/// <summary>
        /// Enables fast, sequential memory allocation without frees from a pre-allocated memory block.        
		/// </summary>
        template<typename Counter> class StackAllocator : public Allocator {
		public:
            StackAllocator() {};
            StackAllocator(Arena arena) {
                Reset(arena);
			}            
            /// <summary>
            /// Resets the stack allocator to the initial state, allowing for reuse of the memory block (Arena)
            /// </summary>
            void Reset(Arena arena) {
                m_memory = arena.memory;
                m_current = reinterpret_cast<size_type>(arena.memory);
                m_end = reinterpret_cast<size_type>(arena.memory) + arena.size;
                m_used = 0;
            }
            /// <summary>
            /// Resets the stack allocator to a non-allocated state.
            /// </summary>
            void Reset() {
                Reset({ nullptr, 0 });
            }
			/// <summary>
            /// Allocates a block of memory of the specified size.
            /// If the requested size exceeds the available memory within the arena, returns nullptr.
			/// </summary>			
			pointer Allocate(size_type size) override;
			/// <summary>
            /// Allocates a block of memory of the specified size with alignment.
            /// If the requested size exceeds the available memory within the arena, returns nullptr.
			/// </summary>		
			pointer Allocate(size_type size, size_type alignment) override;
            /// <summary>
            /// No-op. No memory is modified with this operation.
            /// </summary>
            inline void Deallocate(pointer ptr) { /* nop */ }
            /// <summary>
            /// No-op. No memory is modified with this operation.
            /// </summary>
            inline void Deallocate(pointer ptr, size_type size) {  /* nop */ }
            /// <summary>
            /// Unsupported. Throws std::runtime_error when invoked.
            /// </summary>
            inline pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) {
                throw std::runtime_error("StackAllocator does not support reallocation");
            }
            inline size_type GetUsedMemory() const noexcept override {
                return m_current - reinterpret_cast<size_type>(m_memory);
            }
            constexpr operator bool() const noexcept { return m_memory != nullptr; }
		private:		
            pointer m_memory{ nullptr };
            Counter m_end{};
            Counter m_current{};
            Counter m_used{};
		};
        /// <summary>
        /// Defines a type alias for HeapAllocator using CounterSingleThreaded as the counter type.
        /// Allows for single-threaded allocations and custom Arena memory management.
        /// Allocations being made through this allocator are tracked.
        /// </summary>
        using StackAllocatorSingleThreaded = StackAllocator<CounterSingleThreaded>;
        /// <summary>
        /// Defines a type alias for HeapAllocator that uses atomic counter.
        /// Does NOT allow for custom Arena memory management as heaps are bound to threads.
        /// Allocations being made through this allocator are tracked.
        /// </summary>
        using StackAllocatorMultiThreaded = StackAllocator<CounterMultiThreaded>;
	}
}
