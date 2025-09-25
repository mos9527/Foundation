#pragma once
#include "Core.hpp"
#include <mutex>
namespace Foundation::Core {
    /**
     * @brief Unbounded object pool with O(1) value mapping
     * @note This implementation utilizes locks to guarantee thread safety.
     *       For a lock-free, bounded implementation see @ref Atomics::Pool.
     * @tparam K Key type. Should be an integral type.
     * @tparam V Value type.
     * @tparam Tombstone Tombstone value type for erased values.
     */
    template<typename K, typename V, typename Tombstone = V>
    class Pool {
        Vector<K> mKeys;
        Vector<V> mValues;
        Vector<bool> mBitmap;
        mutable std::recursive_mutex mMutex;
        K mTop{};
        /**
         * @brief Adds a key to the internal key container and resizes the value container if necessary.
         */
        void Push(K key) {
            std::scoped_lock lock(mMutex);
            mKeys.push_back(key);
            if (key >= mValues.size())
                mValues.resize(key + 1);
        }
    public:
        Pool(Allocator* alloc) : mKeys(alloc), mValues(alloc), mBitmap(alloc) {}
        /**
         * @brief Checks if the specified key exists and has a value.
         */
        bool Contains(K key) const {
            std::scoped_lock lock(mMutex);
            return key < mValues.size() && mBitmap[key];
        }
        /**
         * @brief Retrieves a reference to the value associated with the given key.
         * NOTE: Calling this function with a key that's not retrieved from pop() is undefined behavior.
         */
        V& At(K key) {
            std::scoped_lock lock(mMutex);
            CHECK_MSG(Contains(key), "Key not allocated");
            return mValues[key];
        }
        /**
         * @brief Retrieves a const reference to the value associated with the given key.
         */
        V const& At(K key) const {
            std::scoped_lock lock(mMutex);
            CHECK_MSG(Contains(key), "Key not allocated");
            return mValues[key];
        }
        /**
         * @brief Pops (allocates) a Key from the free list and returns it.
         * If the free list is empty, a new key is allocated.
         * The value associated with the key is guaranteed to be zero-initialized.
         */
        K Pop() {
            std::scoped_lock lock(mMutex);
            K key;
            if (mKeys.empty())
                key = mTop++;
            else
                key = mKeys.back(), mKeys.pop_back();
            if (key >= mValues.size())
                mValues.resize(key + 1),
                mBitmap.resize(key + 1);
            return key;
        }
        /**
         * @brief Allocates a Key that returns a pair of key and value reference.
         */
        [[nodiscard]] Pair<K, V&> PopPair()
        {
            std::scoped_lock lock(mMutex);
            K key = Pop();
            mBitmap[key] = true;
            return { key, mValues[key] };
        }
        /**
         * @brief Frees the value associated with the specified key
         */
        void Free(K key) {
            std::scoped_lock lock(mMutex);
            CHECK_MSG(Contains(key), "Key not allocated");
            mValues[key] = Tombstone{};
            mBitmap[key] = false;
            Push(key);
        }
    };
}