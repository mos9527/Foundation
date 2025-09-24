#pragma once
#include "Core.hpp"
#include <mutex>
namespace Foundation::Core {
    /**
     * @brief Unbounded free list with O(1) value mapping
     * @note This implementation utilizes locks to guarantee thread safety.
     *       For a lock-free, bounded implementation see @ref Atomics::FreeList.
     * @tparam K Key type. Should be an integral type.
     * @tparam V Value type.
     * @tparam Tombstone Tombstone value type for erased values.
     */
    template<typename K, typename V, typename Tombstone = V>
    class FreeList {
        Vector<K> m_keys;
        Vector<V> m_values;
        Vector<bool> m_bitmap;
        mutable std::recursive_mutex m_mutex;
        K m_top{};
        /**
         * @brief Adds a key to the internal key container and resizes the value container if necessary.
         */
        void push(K key) {
            std::scoped_lock lock(m_mutex);
            m_keys.push_back(key);
            if (key >= m_values.size())
                m_values.resize(key + 1);
        }
    public:
        FreeList(Allocator* alloc) : m_keys(alloc), m_values(alloc), m_bitmap(alloc) {}
        /**
         * @brief Checks if the specified key exists and has a value.
         */
        bool contains(K key) const {
            std::scoped_lock lock(m_mutex);
            return key < m_values.size() && m_bitmap[key];
        }
        /**
         * @brief Retrieves a reference to the value associated with the given key.
         * NOTE: Calling this function with a key that's not retrieved from pop() is undefined behavior.
         */
        V& at(K key) {
            std::scoped_lock lock(m_mutex);
            CHECK_MSG(contains(key), "Key not allocated");
            return m_values[key];
        }
        /**
         * @brief Retrieves a const reference to the value associated with the given key.
         */
        V const& at(K key) const {
            std::scoped_lock lock(m_mutex);
            CHECK_MSG(contains(key), "Key not allocated");
            return m_values[key];
        }
        /**
         * @brief Pops (allocates) a Key from the free list and returns it.
         * If the free list is empty, a new key is allocated.
         * The value associated with the key is guaranteed to be zero-initialized.
         */
        K pop() {
            std::scoped_lock lock(m_mutex);
            K key;
            if (m_keys.empty())
                key = m_top++;
            else
                key = m_keys.back(), m_keys.pop_back();
            if (key >= m_values.size())
                m_values.resize(key + 1),
                m_bitmap.resize(key + 1);
            return key;
        }
        /**
         * @brief Allocates a Key that returns a pair of key and value reference.
         */
        [[nodiscard]] const Pair<K, V&> pop_pair()
        {
            std::scoped_lock lock(m_mutex);
            K key = pop();
            m_bitmap[key] = true;
            return { key, m_values[key] };
        }
        /**
         * @brief Frees the value associated with the specified key
         */
        void free(K key) {
            std::scoped_lock lock(m_mutex);
            CHECK_MSG(contains(key), "Key not allocated");
            m_values[key] = Tombstone{};
            m_bitmap[key] = false;
            push(key);
        }
    };
}