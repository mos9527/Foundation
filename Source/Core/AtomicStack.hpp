#pragma once
#include "Allocator.hpp"
#include "Atomic.hpp"
namespace Foundation::Core
{
    /**
     * @brief Atomic, unbounded LIFO stack with lock-free push and pop operations.
     * @note Memory allocations are performed on each push and deallocations on each pop.
     *       This also requires a thread-safe Allocator, which @ref Core provides.
     * @note Consider using @ref AtomicPool for fixed-size allocations to reduce allocation overhead
     *       if a max-bound is known.
     * @tparam T Data type, must be default constructible.
     */
    template <typename T>
    class AtomicStack
    {
    public:
        struct Node;
        struct alignas(2 * sizeof(Node*)) PTag
        {
            Node* p{nullptr};
            uintptr_t tag{};
        };
        struct Node
        {
            PTag next{};
            T data{};
        };

    private:
        Atomic<PTag> mTop{};
        Allocator* mAlloc;

    public:
        /**
         * @brief Construct the Stack.
         * @param alloc Allocator to use for element allocations.
         */
        AtomicStack(Allocator* alloc) : mAlloc(alloc) {}
        /**
         * @brief Push a value onto the stack.
         * @note Multiple threads may call this concurrently.
         * @tparam U Type of the value to push. May be different from T, but must be convertible to T.
         * @param value The value to push. This is forwarded to T's constructor.
         */
        template <typename U>
        void Push(U&& value)
        {
            Node* node = static_cast<Node*>(mAlloc->Allocate(sizeof(Node), alignof(Node)));
            node->data = std::forward<U>(value);
            // Node* old_top = m_top.load(std::memory_order_relaxed);
            // -> Doesn't avoid https://en.wikipedia.org/wiki/ABA_problem - a pushes and a pop then a push
            //    effectively make the new pointer the same again on linear allocators, but it's a different node.
            //    CAS later won't solve this. We need to tag the pointer.
            PTag old_top = mTop.load(std::memory_order_relaxed);
            node->next = old_top;
            // -> Tag is incremented on every modification of the head pointer.
            while (true)
            {
                PTag new_top{node, old_top.tag + 1};
                if (mTop.compare_exchange_weak(old_top, new_top, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    node->next = old_top;
                    return; // OK. Acquired the head and we're in
                }
            }
        }
        /**
         * @brief Pop a value from the stack.
         * @param out Reference to receive the popped value. This is only valid if the function returns true.
         * @note Multiple threads may call this concurrently.
         * @return True if a value was popped, false if the stack was empty.
         */
        bool Pop(T& out)
        {
            PTag old_top = mTop.load(std::memory_order_relaxed);
            while (old_top.p != nullptr)
            {
                // Same as above
                PTag new_top{old_top.p->next.p, old_top.tag + 1};
                if (mTop.compare_exchange_weak(old_top, new_top, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    out = std::move(old_top.p->data);
                    old_top.p->~Node();
                    mAlloc->Deallocate(old_top.p);
                    return true; // OK. Acquired the head and we're out
                }
            }
            return false; // Empty
        }
        ~AtomicStack()
        {
            T val;
            while (Pop(val))
                ;
        }
    };
} // namespace Foundation::Core
