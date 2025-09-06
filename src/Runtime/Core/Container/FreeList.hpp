#pragma once
#include "Common.hpp"

namespace Foundation::Core {
    /**
     * @brief A free list counter implementation with amortized O(1) allocation and deallocation.
     */
    template<typename T = uint64_t> class FreeListCounter {
        StlVector<T> m_free;
        T m_top = 0;
        size_t m_allocated{ 0 };
    public:
        void reserve(size_t size) {
            m_free.reserve(size);
        }
        FreeListCounter(Allocator* alloc) : m_free(alloc) {}
        /**
         * @brief Pops a value from the free list.
         * If the free list is empty, a new key is allocated.
         */
        T pop() {
            if (m_free.empty())
                m_free.push_back(m_top++);
            T value = m_free.back();
            m_free.pop_back();
            m_allocated++;
            return value;
        }
        void push(T value) {
            m_free.push_back(value);
            m_allocated--;
        }
        size_t size() const {
            return m_free.size();
        }
        void clear() {
            m_free.clear();
            m_top = 0;
            m_allocated = 0;
        }
        size_t allocation() const {
            return m_allocated;
        }
    };
    /**
     * @brief A dense map implementation based on free list with amortized O(1) allocation and deallocation.
     * This is by no means a conventional associative container, nor should it be used as such.
     * For all intents and purposes, this is, and SHOULD be used as an *Object Pool*
     * where allocation and deallocation of keys is done in a LIFO manner.
     */
    template<typename K, typename V>
    class FreeList {
        FreeListCounter<K> m_keys;
        StlVector<V> m_values;
        StlVector<bool> m_bitmap;
        /**
         * @brief Adds a key to the internal key container and resizes the value container if necessary.
         */
        void push(K key) {
            m_keys.push(key);
            if (key >= m_values.size())
                m_values.resize(key + 1);
        }
        /**
         * @brief Pops (allocates) a Key from the free list and returns it.
         * If the free list is empty, a new key is allocated.
         * The value associated with the key is guaranteed to be zero-initialized.
         */
        K pop() {
            K key = m_keys.pop();
            if (key >= m_values.size())
                m_values.resize(key + 1),
                m_bitmap.resize(key + 1);
            return key;
        }
    public:
        void reserve(size_t size) {
            m_keys.reserve(size);
            m_values.reserve(size);
            m_bitmap.reserve(size);
        }
        FreeList(Allocator* alloc) : m_keys(alloc), m_values(alloc), m_bitmap(alloc) {}
        FreeList(Allocator* alloc, size_t reserve_size) : FreeList(alloc) {
            reserve(reserve_size);
        }
        /**
         * @brief Retrieves a reference to the value associated with the given key.
         * NOTE: Calling this function with a key that's not retrieved from pop() is undefined behavior.
         */
        V& at(K key) {
            return m_values[key];
        }
        /**
         * @brief Retrieves a const reference to the value associated with the given key.
         * NOTE: Calling this function with a key that's not retrieved from pop() is undefined behavior.
         */
        V const& at(K key) const {
            return m_values[key];
        }
        /**
         * @brief Checks if the specified key exists and has a value.
         */
        bool contains(K key) const {
            return key < m_values.size() && m_bitmap[key];
        }
        /**
         * @brief Allocates a Key that returns a pair of key and value reference.
         */
        const std::pair<K, V&> allocate() {
            K key = pop();
            m_bitmap[key] = true;
            return { key, m_values[key] };
        }
        /**
         * @brief Frees the value associated with the specified key
         */
        void free(K key) {
            m_values[key] = {};
            m_bitmap[key] = false;
            push(key);
        }
        size_t size() const {
            return m_keys.size();
        }
        void clear() {
            m_keys.clear();
            m_values.clear();
            m_bitmap.clear();
        }
        size_t allocation() const {
            return m_keys.allocation();
        }
    };
}
