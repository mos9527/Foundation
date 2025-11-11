#pragma once
#include <cstring>


#include "Allocator.hpp"
#include "Atomic.hpp"
namespace Foundation::Core
{
    /**
     * @brief Implements an O(1) lock-free pool allocator/object pool of fixed allocation sizes.
     * @note This implementation is thread-safe for allocations from multiple threads.
     * @tparam T Type of object to allocate, must be default constructible.
     */
    template <typename T>
    class AllocatorPool : public Allocator
    {
        struct Node;
        struct alignas(2 * sizeof(Node*)) PTag
        {
            Node* p{ nullptr };
            uintptr_t tag{};
        };
        struct Node
        {
            Node* next;
            T data;
        };
        Node* mNodes { nullptr };
        size_t mSize { 0 };
        Atomic<PTag> mHead{};
    public:
        static constexpr size_t kBlockSize = sizeof(Node);
        static consteval size_t SizeOfObjects(size_t count) {
            return count * kBlockSize;
        }

        AllocatorPool() = default;
        AllocatorPool(Arena arena) : mSize(arena.size / kBlockSize)
        {
            std::memset(arena.memory, 0, arena.size);
            mNodes = static_cast<Node*>(arena.memory);
            for (size_t i = 0; i + 1 < mSize; i++)
                mNodes[i].next = &mNodes[i + 1];
            mNodes[mSize - 1].next = nullptr;
            mHead.store({ &mNodes[0], 0});
        }
        pointer Allocate()
        {
            PTag old_head = mHead.load(std::memory_order_relaxed);
            while (true)
            {
                if (!old_head.p)
                    return nullptr;
                // ABA mitigation as seen in AtomicStack
                PTag new_head = { old_head.p->next, old_head.tag + 1 };
                if (mHead.compare_exchange_weak(old_head, new_head, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    return &old_head.p->data;
                }
            }
        }
        pointer Allocate(size_type size, size_type alignment) override
        {
            if (size != sizeof(T)) [[unlikely]]
                throw std::runtime_error("AllocatorPool can only allocate sizes of type T size");
            return Allocate();
        }
        template<typename ...Args> T* AllocateObject(Args&&... args)
        {
            T* ptr = static_cast<T*>(Allocate());
            std::construct_at(ptr, std::forward<Args>(args)...);
            return ptr;
        }
        void Deallocate(pointer ptr) override {
            PTag old_head = mHead.load(std::memory_order_relaxed);
            // We know the memory layout is Node.data, so we can get the Node* from T*
            // Hacky - though guaranteed to work by standard.
            Node* node = reinterpret_cast<Node*>(static_cast<uint8_t*>(ptr) - sizeof(Node::next));
            while (true)
            {
                node->next = old_head.p;
                PTag new_head{ node, old_head.tag + 1 };
                if (mHead.compare_exchange_weak(old_head, new_head, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    node->data = T{};
                    // Reset data to default state
                    // This is used to effectively mark pointers as "freed"
                    // so GC at @ref Collect doesn't try to destruct them again.
                    // In hindsight, a separate bitmap would be more efficient.
                    return;
                }
            }
        }
        /**
         * @note Unsupported. Throws std::runtime_error when invoked.
         */
        pointer Reallocate(pointer ptr, size_type new_size, size_t alignment) override
        {
            throw std::runtime_error("Allocator does not support reallocation");
        }
        /**
         * @brief Destruct all allocated objects in the pool, collecting garbage.
         */
        void Collect()
        {
            for (size_t i = 0; i < mSize; i++)
                mNodes[i].data.~T();
        }
    };
} // namespace Foundation::Core
