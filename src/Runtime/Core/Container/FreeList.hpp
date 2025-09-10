#pragma once
#include "Common.hpp"

namespace Foundation::Core {
    /**
     * @brief A free list counter implementation with amortized O(1) allocation and deallocation.
     */
    template<typename T = uint64_t> class FreeListCounter {
        Vector<T> m_free;
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
     * @brief A dense map implementation with amortized O(1) allocation and deallocation.
     *
     * Reallocation (growth) behaviour is the same as std::vector.
     */
    template<typename K, typename V>
    class FreeList {
        FreeListCounter<K> m_keys;
        Vector<V> m_values;
        Vector<bool> m_bitmap;
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
        K allocate() {
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
        FreeList(size_t reserve_size, Allocator* alloc) : FreeList(alloc) {
            reserve(reserve_size);
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
        /**
         * @brief Checks if the specified key exists and has a value.
         */
        bool contains(K key) const {
            return key < m_values.size() && m_bitmap[key];
        }
        /**
         * @brief Allocates a Key that returns a pair of key and value reference.
         */
        [[nodiscard]] const Pair<K, V&> pop()
        {
            K key = allocate();
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
            return m_keys.allocation();
        }
        void clear() {
            m_keys.clear();
            m_values.clear();
            m_bitmap.clear();
        }
    };
}
