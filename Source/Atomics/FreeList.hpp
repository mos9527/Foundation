#pragma once
#include <Core/Container.hpp>
#include "Queue.hpp"
namespace Foundation::Atomics {
    using namespace Core;
    /**
     * @brief Atomic, bounded free list with O(1) value mapping
     * @tparam K Key type. Should be an integral type.
     * @tparam V Value type.
     * @tparam Tombstone Tombstone value type for erased values.
     */
    template<typename K, typename V, typename Tombstone = V>
    class FreeList {
        const size_t m_capacity;
        MPMCQueue<K> m_keys;
        Vector<V> m_values;
        // Yes - there is Vector<bool>. If only you know the horror to
        // synchronize a bitset across threads...
        Vector<char> m_bitmap;
    public:
        /**
         * @brief Constructs a FreeList with the specified capacity and allocator.
         * @param size The maximum number of elements the FreeList can hold, which will be pre-allocated.
         * @param alloc Allocator to use for internal allocations.
         */
        FreeList(size_t size, Allocator* alloc) :
            m_capacity(size), m_keys(size, alloc), m_values(size, alloc), m_bitmap(size, alloc)
        {
            // Allocate all the keys that can be possibly produced
            auto writer = m_keys.create_writer();
            for (size_t i = 0; i < size; i++)
                writer.push(i);
        }
        /**
         * @brief Checks if the specified key exists and has a value.
         */
        bool contains(K key) const {
            return key < m_values.size() && m_bitmap[key];
        }
        /**
         * @brief Retrieves a reference to the value associated with the given key.
         * NOTE: Calling this function with a key that's not retrieved from pop() is undefined behavior.
         */
        V& at(K key) {
            CHECK_MSG(contains(key), "Key not allocated");
            return m_values[key];
        }
        /**
         * @brief Retrieves a const reference to the value associated with the given key.
         */
        V const& at(K key) const {
            CHECK_MSG(contains(key), "Key not allocated");
            return m_values[key];
        }
        const K pop()
        {
            K key;
            CHECK_MSG(m_keys.create_reader().pop(key), "Key pool exhausted");
            m_bitmap[key] = true;
            return key;
        }
        /**
         * @brief Allocates a Key that returns a pair of key and value reference.
         */
        [[nodiscard]] const Pair<K, V&> pop_pair()
        {
            K key = pop();
            m_bitmap[key] = true;
            return { key, m_values[key] };
        }
        /**
         * @brief Frees the value associated with the specified key
         */
        void free(K key) {
            CHECK_MSG(contains(key), "Key {} is invalid", key);
            m_values[key] = Tombstone{};
            m_bitmap[key] = false;
            CHECK_MSG(m_keys.create_writer().push(key), "Key pool full");
        }
    };
}
