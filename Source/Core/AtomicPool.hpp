#pragma once
#include <cstring>


#include "Allocator.hpp"
#include "Atomic.hpp"
namespace Foundation::Core
{
    /**
     * @brief Atomic, bounded object pool of fixed allocation sizes.
     *        Being a sibling to @ref AtomicStack - key differences being how objects are
     *        allocated in a fixed-size arena and is never freed back to the system.
     *        Deallocation returns objects to the pool for reuse.
     * @note This implementation is thread-safe for allocations from multiple threads.
     * @tparam T Type of object to allocate, must be default constructible.
     */
    template <typename T>
    class AtomicPool
    {
        struct Node;
        struct alignas(2 * sizeof(Node*)) PTag
        {
            Node* p{nullptr};
            uintptr_t tag{};
        };
        struct Node
        {
            Node* next;
            T data;
            ///
            bool used{false};
        };
        Node* mNodes{nullptr};
        size_t mSize{0};
        Atomic<PTag> mHead{};
        ScopedArena mArena;

        Node* AllocateNode()
        {
            PTag old_head = mHead.load(std::memory_order_relaxed);
            while (true)
            {
                if (!old_head.p)
                    return nullptr;
                // ABA mitigation as seen in AtomicStack
                PTag new_head = {old_head.p->next, old_head.tag + 1};
                if (mHead.compare_exchange_weak(old_head, new_head, std::memory_order_acquire,
                                                std::memory_order_relaxed))
                {
                    return old_head.p;
                }
            }
        }

        void DeallocateNode(Node* node)
        {
            PTag old_head = mHead.load(std::memory_order_relaxed);
            while (true)
            {
                node->next = old_head.p;
                PTag new_head{node, old_head.tag + 1};
                if (mHead.compare_exchange_weak(old_head, new_head, std::memory_order_acquire,
                                                std::memory_order_relaxed))
                {
                    return;
                }
            }
        }

    public:
        AtomicPool() = default;
        AtomicPool(size_t size, Allocator* alloc) : mSize(size), mArena(alloc, size * sizeof(Node))
        {
            std::memset(mArena.arena.memory, 0, mArena.arena.size);
            mNodes = static_cast<Node*>(mArena.arena.memory);
            for (size_t i = 0; i + 1 < mSize; i++)
                mNodes[i].next = &mNodes[i + 1];
            mNodes[mSize - 1].next = nullptr;
            mHead.store({&mNodes[0], 0});
        }

        template <typename... Args>
        T* Allocate(Args&&... args)
        {
            Node* node = AllocateNode();
            node->used = true;
            std::construct_at(&node->data, std::forward<Args>(args)...);
            return &node->data;
        }
        void Deallocate(T* ptr)
        {
            // We know the memory layout is Node.data, so we can get the Node* from T*
            // Hacky - though guaranteed to work by standard.
            Node* node = reinterpret_cast<Node*>(reinterpret_cast<uint8_t*>(ptr) - sizeof(Node::next));
            node->data.~T();
            node->used = false;
            DeallocateNode(node);
        }
        /**
         * @brief Destruct all allocated objects in the pool, collecting garbage.
         */
        void Collect()
        {
            for (size_t i = 0; i < mSize; i++)
                if (mNodes[i].used)
                    mNodes[i].data.~T();
        }
        ~AtomicPool() { Collect(); }
    };
} // namespace Foundation::Core
