#pragma once
#include <vector>
#include <numeric>
#include <optional>

#include "Common.hpp"
namespace Foundation::Core {
    /// <summary>
    /// A free list counter implementation with amortized O(1) allocation and deallocation.
    /// </summary>
    template<typename T = uint64_t> class FreeListCounter {
        StlVector<T> m_free;
        T m_top = 0;
        size_t m_allocated{ 0 };
    public:
        void reserve(size_t size) {
            m_free.reserve(size);
        }
        FreeListCounter(Allocator* alloc) : m_free(alloc) {}
        /// <summary>
        /// Pops a value from the free list.
        /// If the free list is empty, a new key is allocated.
        /// </summary>
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
    /// <summary>
    /// A dense map implementation based on free list with amortized O(1) allocation and deallocation.
    /// This is by no means a conventional associative container, nor should it be used as such.
    /// For all intents and purposes, this is, and SHOULD be used as an *Object Pool*
    /// where allocation and deallocation of keys is done in a LIFO manner.
    /// </summary>
    template<typename K, typename V>
    class FreeDenseMap {
        FreeListCounter<K> m_keys;
        StlVector<V> m_values;
        /// <summary>
        /// Adds a key to the internal key container and resizes the value container if necessary.
        /// </summary>
        void push(K key) {
            m_keys.push(key);
            if (key >= m_values.size())
                m_values.resize(key + 1);
        }
    public:
        void reserve(size_t size) {
            m_keys.reserve(size);
            m_values.reserve(size);
        }
        FreeDenseMap(Allocator* alloc) : m_keys(alloc), m_values(alloc) {}
        FreeDenseMap(Allocator* alloc, size_t reserve_size) : FreeDenseMap(alloc) {
            reserve(reserve_size);
        }
        /// <summary>
        /// Pops (allocates) a Key from the free list and returns it.
        /// If the free list is empty, a new key is allocated.
        /// The value associated with the key is guaranteed to be zero-initialized.
        /// </summary>
        K pop() {
            K key = m_keys.pop();
            if (key >= m_values.size())
                m_values.resize(key + 1);
            return key;
        }
        /// <summary>
        /// Retrieves a reference to the value associated with the given key.
        /// NOTE: Calling this function with a key that's not retrieved from pop() is undefined behavior.
        /// </summary>
        V& at(K key) {
            return m_values[key];
        }
        /// <summary>
        /// Retrieves a const reference to the value associated with the given key.
        /// NOTE: Calling this function with a key that's not retrieved from pop() is undefined behavior.
        /// </summary>
        V const& at(K key) const {
            return m_values[key];
        }
        /// <summary>
        /// Checks if the specified key exists and has a value.
        /// </summary>
        bool contains(K key) const {
            return key < m_values.size() && m_values[key].get() != nullptr;
        }
        /// <summary>
        /// Allocates a Key that returns a pair of key and value reference.
        /// </summary>        
        const std::pair<K, V&> allocate() {
            K key = pop();
            return { key, m_values[key] };
        }
        /// <summary>
        /// Frees the value associated with the specified key
        /// </summary>
        void free(K key) {
            m_values[key] = {};
            push(key);
        }
        size_t size() const {
            return m_keys.size();
        }
        void clear() {
            m_keys.clear();
            m_values.clear();
        }
        size_t allocation() const {
            return m_keys.allocation();
        }
    };
}
