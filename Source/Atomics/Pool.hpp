#pragma once
#include <Core/Container.hpp>
#include "Queue.hpp"
namespace Foundation::Atomics {
    using namespace Core;
    /**
     * @brief Atomic, bounded object pool with O(1) value mapping
     * @note This is the atomic, lock-free version of @ref Core::Pool.
     *       The pool has a fixed maximum size and will never allocate memory after construction.
     *       For an unbounded, lock-based implementation see @ref Core::Pool.
     * @tparam K Key type. Should be an integral type.
     * @tparam V Value type.
     * @tparam Tombstone Tombstone value type for erased values.
     */
    template<typename K, typename V, typename Tombstone = V>
    class Pool {
        const size_t mCapacity;
        MPMCQueue<K> mKeys;
        Vector<V> mValues;
        // Yes - there is Vector<bool>. If only you know the horror to
        // synchronize a bitset across threads...
        Vector<char> mBitmap;
    public:
        /**
         * @brief Constructs a FreeList with the specified capacity and allocator.
         * @param size The maximum number of elements the FreeList can hold, which will be pre-allocated.
         * @param alloc Allocator to use for internal allocations.
         */
        Pool(size_t size, Allocator* alloc) :
            mCapacity(size), mKeys(size, alloc), mValues(size, alloc), mBitmap(size, alloc)
        {
            // Allocate all the keys that can be possibly produced
            auto writer = mKeys.CreateWriter();
            for (size_t i = 0; i < size; i++)
                writer.Push(i);
        }
        /**
         * @brief Checks if the specified key exists and has a value.
         */
        bool Contains(K key) const {
            return key < mValues.size() && mBitmap[key];
        }
        /**
         * @brief Retrieves a reference to the value associated with the given key.
         * NOTE: Calling this function with a key that's not retrieved from pop() is undefined behavior.
         */
        V& At(K key) {
            CHECK_MSG(Contains(key), "Key not allocated");
            return mValues[key];
        }
        /**
         * @brief Retrieves a const reference to the value associated with the given key.
         */
        V const& At(K key) const {
            CHECK_MSG(Contains(key), "Key not allocated");
            return mValues[key];
        }
        const K Pop()
        {
            K key;
            CHECK_MSG(mKeys.CreateReader().Pop(key), "Key pool exhausted");
            mBitmap[key] = true;
            return key;
        }
        /**
         * @brief Allocates a Key that returns a pair of key and value reference.
         */
        [[nodiscard]] const Pair<K, V&> PopPair()
        {
            K key = Pop();
            mBitmap[key] = true;
            return { key, mValues[key] };
        }
        /**
         * @brief Frees the value associated with the specified key
         */
        void Free(K key) {
            CHECK_MSG(Contains(key), "Key {} is invalid", key);
            mValues[key] = Tombstone{};
            mBitmap[key] = false;
            CHECK_MSG(mKeys.CreateWriter().Push(key), "Key pool full");
        }
    };
}
