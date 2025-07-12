#pragma once
#include <vector>
#include <numeric>
#include <optional>

#include <Core/Allocator/Allocator.hpp>
namespace Foundation {
    namespace Core {
        namespace Bits {
            template<typename T> class FreeList {
                std::vector<T, StlAllocator<T>> m_free;
                T m_top = 0;
            public:
                FreeList(Allocator* alloc) : m_free(StlAllocator<T>(alloc)) {}
                /// <summary>
                /// Pops a value from the free list.
                /// If the free list is empty, a new key is allocated.
                /// </summary>
                /// <returns></returns>
                T pop() {
                    if (m_free.empty())
                        push(m_top++);
                    T value = m_free.back();
                    m_free.pop_back();
                    return value;
                }
                void push(T value) {
                    m_free.push_back(value);
                }
            };

            template<typename K, typename V>
            class FreeDenseMap {                
                using value_type = UniquePtr<V>;

                FreeList<K> m_keys;
                std::vector<value_type, StlAllocator<value_type>> m_values;
                /// <summary>
                /// Adds a key to the internal key container and resizes the value container if necessary.
                /// </summary>
                void push(K key) {
                    m_keys.push(key);
                    if (key >= m_values.size())
                        m_values.resize(key + 1);
                }
            public:
                FreeDenseMap(Allocator* alloc) : m_keys(alloc), m_values(StlAllocator<value_type>(alloc)) {}
                /// <summary>
                /// Pops a Key from the free list and returns it.
                /// If the free list is empty, a new key is allocated.
                /// The value associated with the key is guaranteed to be initialized, and maybe modified.
                /// </summary>
                K pop() {
                    K key = m_keys.pop();
                    if (key >= m_values.size())
                        m_values.resize(key + 1);
                    return key;
                }
                /// <summary>
                /// Retrieves a reference to the value associated with the given key.
                /// </summary>
                value_type& at(K key) {
                    return m_values[key];
                }
                value_type const& at(K key) const {
                    return m_values[key];
                }
                /// <summary>
                /// Checks if the specified key exists and has a value.
                /// </summary>
                bool contains(K key) const {
                    return key < m_values.size() && m_values[key].get() != nullptr;
                }
                /// <summary>
                /// Erases the value associated with the specified key
                /// </summary>
                void erase(K key) {
                    m_values[key].reset();
                    push(key);
                }
            };
        }
    }
}
