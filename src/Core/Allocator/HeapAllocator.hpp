#pragma once
#include <Core/Allocator/Allocator.hpp>
#include <mimalloc.h>
namespace Foundation {
	namespace Core {
		/// <summary>
        /// Heap allocator based on mimalloc's heaps.
        /// <summary>
		template<typename Counter> class HeapAllocator : public Allocator {
            mi_heap_t* m_heap{nullptr};
            mi_arena_id_t mi_arena{ nullptr };
            Counter m_used{};
		public:
            constexpr static size_type kHeapMaxAllocation = 16 * 1024 * 1024; // 16 MB
            /// <summary>
            /// Constructs a heap allocator that uses the default mimalloc heap.
            /// </summary>
            HeapAllocator() = default;
            /// <summary>
            /// Constructs a heap allocator that uses the specified arena for allocations.
            /// This is only available for single-threaded implementations.
            /// </summary>            
            HeapAllocator(Arena arena) requires std::is_same_v<Counter, CounterSingleThreaded> {
                if (reinterpret_cast<size_type>(arena.memory) % (1LL << 16) != 0) {
                    throw std::runtime_error("Arena memory must be aligned to 64 KiB");
                }
                RUNTIME_GUARD(mi_manage_os_memory_ex(
                    arena.memory,
                    arena.size,
                    true, /* comitted */
                    false, /* pinned */
                    false, /* zero */
                    -1,
                    true, /* exclusive */
                    &mi_arena /* arena id */
                ), "mimalloc failed to reserve memory");
                m_heap = mi_heap_new_in_arena(mi_arena);
            }
            ~HeapAllocator() {
                if (m_heap)
                    mi_heap_delete(m_heap);
            }

            /// <summary>
            /// Allocates a block of memory of the specified size.
            /// If the requested size exceeds the available memory within the heap's arena, returns nullptr.
            /// If the requested size exceeds kHeapMaxAllocation, allocation fallbacks to mi_malloc
            /// and is not kept on the heap.
            /// </summary>	
            pointer Allocate(size_type size) override;
            /// <summary>
            /// Allocates a block of memory of the specified size with alignment.
            /// If the requested size exceeds the available memory within the heap's arena, returns nullptr.
            /// If the requested size exceeds kHeapMaxAllocation, allocation fallbacks to mi_malloc
            /// and is not kept on the heap.
            /// </summary>	
            pointer Allocate(size_type size, size_t alignment) override;

            void Deallocate(pointer ptr) override;
            inline void Deallocate(pointer ptr, size_type size) { Deallocate(ptr); }

            /// <summary>
            /// Reallocates a block of memory.
            /// If the new size exceeds the available memory within the heap's arena, allocation
            /// fallbacks to mi_realloc_aligned and is not kept on the heap.
            /// </summary>
            pointer Reallocate(pointer ptr, size_type new_size, size_t alignment);

            inline size_type GetUsedMemory() const noexcept override { return m_used; }
		};
        /// <summary>
        /// Defines a type alias for HeapAllocator using CounterSingleThreaded as the counter type.
        /// For allocations lesser or equal to kHeapMaxAllocation, allocations are made through the custom heap.
        /// Larger allocations fallback to mimalloc's default allocation behaviors.
        /// (see also https://github.com/microsoft/mimalloc/issues/980)
        /// Allocations being made through this allocator are tracked.
        /// Allows for single-threaded allocations and custom Arena memory management.
        /// </summary>
        using HeapAllocatorSingleThreaded = HeapAllocator<CounterSingleThreaded>;
        /// <summary>
        /// Defines a type alias for HeapAllocator that uses atomic counter.
        /// Serves as a thin wrapper around mimalloc's default allocation behaviors,
        /// which is thread-safe by default.
        /// Allocations being made through this allocator are tracked.
        /// Does NOT allow for custom Arena memory management as heaps are bound to threads.
        /// </summary>
        using HeapAllocatorMultiThreaded = HeapAllocator<CounterMultiThreaded>;
	}
}
