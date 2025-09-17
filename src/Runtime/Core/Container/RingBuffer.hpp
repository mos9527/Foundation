#pragma once
#include "Common.hpp"
namespace Foundation::Core
{
    /**
     * @brief 'Mirrored' circular buffer with a fixed maximum size
     * with O(1) insertion and _random access_ support.
     *
     * @note This is _not_ thread-safe, nor should it be used
     * in a multithreaded context if you want contiguous ranges anyway.
     * For that - see @ref SPSCRingBuffer.
     */
    template <typename T>
    class RingBuffer
    {
        Vector<T> m_buffer;
        long long m_head{ 0 }, m_tail{ 0 };
        const long long m_size;
    public:
        RingBuffer(const size_t size, Allocator* alloc) : m_buffer((size + 1) << 1LL, alloc), m_size(size + 1) {};
        T& emplace_back(const T& value) {
            // Wrap around
            if (m_tail == m_size << 1LL)
                m_head = 1, m_tail = m_size + 1;
            T& ref = m_buffer[m_tail++] = value;
            m_head = std::max(0LL, m_tail + 1LL - m_size);
            // Mirror to the first half
            if (m_head > 0)
                m_buffer[m_head - 1] = value;
            return ref;
        }
        void pop_front()
        {
            if (size() > 0)
                m_head++;
        }
        void pop_back()
        {
            if (size() > 0)
                m_tail--;
        }

        constexpr bool empty() const { return size() == 0; }
        constexpr size_t size() const { return m_tail - m_head; }

        T* data() { return &m_buffer[m_head]; }
        const T* data() const { return &m_buffer[m_head]; }

        Vector<T>::iterator begin() { return m_buffer.begin() + m_head; }
        Vector<T>::iterator end() { return m_buffer.begin() + m_tail; }
        Vector<T>::const_iterator cbegin() { return m_buffer.cbegin() + m_head; }
        Vector<T>::const_iterator cend() { return m_buffer.cbegin() + m_tail; }
    };
}