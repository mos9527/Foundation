#pragma once
#include <Core/Core.hpp>
namespace Foundation::Atomic
{
    using Foundation::Core;
    /**
     * @brief Atomic single-producer single-consumer ring buffer with a fixed maximum size
     *
     * References:
     * - When Nanoseconds Matter: Ultrafast Trading Systems in C++ - David Gross - CppCon 2024
     *   - Link: https://www.youtube.com/watch?v=K3P_Lmq6pw0
     * - Single Producer Single Consumer Lock-free FIFO From the Ground Up - Charles Frasch - CppCon 2023
     *   - Link: https://www.youtube.com/watch?v=sX2nF1fW7kI&t=3374s
     */
    template <typename T>
    class SPSCRingBuffer
    {
        Vector<T> m_buffer;
        long long m_head{ 0 }, m_tail{ 0 };
        const long long m_size;
    public:
        SPSCRingBuffer(size_t capacity, Allocator* alloc) : m_buffer(capacity + 1, alloc), m_size(capacity + 1) {};
    };
}