#pragma once
#include "Core.hpp"
#include <mutex>
namespace Foundation::Core {
    /**
     * @brief Unbounded object pool with O(1) value mapping
     * @tparam K Key type. Should be an integral type.
     * @tparam V Value type.     
     * @note The values are allocated using the provided Allocator, and is guaranteed
     *       to have a stable address until freed.
     * @note An atomic, bounded version of this is provided by @ref Foundation::Atomics::Pool
     */
    template<typename K, typename V>
    class Pool {
        Allocator* mAlloc;
        Vector<K> mKeys;
        mutable Vector<UniquePtr<V>> mValues;        
        mutable std::mutex mMutex;
        K mTop{};
        /**
         * @brief Adds a key to the internal key container and resizes the value container if necessary.
         */
        void Push(K key) {            
            mKeys.push_back(key);
            if (key >= mValues.size())
                mValues.resize(key + 1);
        }     
        void CheckContains(K key) const {
            CHECK_MSG(key < mValues.size() && mValues[key], "Key not allocated"); 
        }
        K PopKey() {
            K key{};
            if (mKeys.empty())
                key = mTop++;
            else
                key = mKeys.back(), mKeys.pop_back();
            if (key >= mValues.size())
                mValues.resize(key + 1);
            return key;
        }
    public:
        Pool(Allocator* alloc) : mAlloc(alloc), mKeys(alloc), mValues(alloc) {}
        /**
         * @brief Checks if the specified key exists and has a value.
         */
        bool Contains(K key) const {
            std::unique_lock lock(mMutex);
            return key < mValues.size() && mValues[key];
        }
        /**
         * @brief Retrieves a reference to the value associated with the given key.
         * NOTE: Calling this function with a key that's not retrieved from pop() is undefined behavior.
         */
        V* At(K key) const {
            std::unique_lock lock(mMutex);
            CheckContains(key);
            return mValues[key].get();
        }
        /**
        * @brief Allocates a new key in the pool and constructs the object in-place.
        */
        template <typename... Args>
        K Pop(Args&&... args)
        {
            std::unique_lock lock(mMutex);
            K key = PopKey();
            mValues[key] = ConstructUnique<V>(mAlloc, std::forward<Args>(args)...);
            return key;
        }
        /**
         * @brief Allocates a new key in the pool and constructs a _derived_ object in-place.
         * @tparam Derived The derived type to construct, based on the base type V.
         */
        template <typename Derived, typename... Args>
        K PopBase(Args&&... args)
        {
            std::unique_lock lock(mMutex);
            K key = PopKey();
            mValues[key] = ConstructUniqueBase<V, Derived>(mAlloc, std::forward<Args>(args)...);
            return key;
        }
        /**
         * @brief Frees the value associated with the specified key
         */
        void Free(K key) {
            std::unique_lock lock(mMutex);
            CheckContains(key);
            mValues[key].reset();            
            Push(key);
        }
    };
}