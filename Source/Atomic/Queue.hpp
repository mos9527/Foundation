#pragma once
#include <Core/Core.hpp>
#include <Core/Allocator.hpp>
#include "Atomic.hpp"
/**
 * @brief Lock-free atomic implementations of data structures.
 */
namespace Foundation::Atomic
{
    using namespace Foundation::Core;
    /**
     * @brief Atomic single-producer single-consumer ring buffer with a fixed maximum size
     *
     * References:
     * - Single Producer Single Consumer Lock-free FIFO From the Ground Up - Charles Frasch - CppCon 2023
     *   - Link: https://www.youtube.com/watch?v=K3P_Lmq6pw0
     */
    template <typename T>
    class SPSCQueue
    {
        const size_t m_modulo;
        Vector<T> m_buffer;
        Atomic<size_t> m_read{}, m_write{};
        // Only used in reader thread
        size_t m_readCached{};
        // Only used in writer thread
        size_t m_writeCached{};
    public:
        /**
         * @brief Construct the SPSC Queue.
         * @param size Bounded size of the queue. Must be a power of two.
         * @param alloc Allocator to use for internal storage.
         */
        SPSCQueue(size_t size, Allocator* alloc) :
            m_modulo(size - 1), m_buffer(size, alloc) {
            CHECK_MSG(size > 0 && (size & m_modulo) == 0, "Size must be a power of two");
        }
        /**
         * @brief _Try_ to push data into the queue.
         * @note Caller MUST guarantee there is only _one_ concurrent thread calling push.
         * @param data The data to push.
         * @return Whether the push was successful. Returns false if the queue is full.
         */
        bool push(T const& data)
        {
            size_t write = m_write.load(std::memory_order_relaxed);
            // Amortize atomic reads
            if (write - m_readCached == m_buffer.size()) [[unlikely]] /* wrapped, update reader */
            {
                m_readCached = m_read.load(std::memory_order_acquire);
                if (write - m_readCached == m_buffer.size())
                    return false; // full
            }
            m_buffer[write & m_modulo] = data;
            m_write.store(write + 1, std::memory_order_release);
            return true;
        }
        /**
         * @brief _Try_ to pop data from the queue.
         * @note Caller MUST guarantee there is only _one_ concurrent thread calling pop.
         * @return An optional containing the popped data, or empty if the queue is empty.
         */
        Optional<T> pop()
        {
            size_t read = m_read.load(std::memory_order_relaxed);
            // Same as above
            if (read == m_writeCached) [[unlikely]]
            {
                m_writeCached = m_write.load(std::memory_order_acquire);
                if (read == m_writeCached)
                    return {}; // empty
            }
            T data = m_buffer[read & m_modulo];
            m_read.store(read + 1, std::memory_order_release);
            return data;
        }
    };
    /**
     * @brief Atomic single-producer multi-consumer ring buffer with a fixed maximum size
     *
     * References:
     * - When Nanoseconds Matter: Ultrafast Trading Systems in C++ - David Gross - CppCon 2024
     *   - Link: https://www.youtube.com/watch?v=sX2nF1fW7kI
     */
    template<typename T>
    class SPMCQueue
    {
        const size_t m_modulo;
        Vector<T> m_buffer;
        Atomic<size_t> m_read{}, m_write{};
        // Only used in writer thread
        size_t m_writeCached{};
    public:
        class Reader
        {
            SPMCQueue* m_queue;
            size_t m_localRead;
        public:
            Optional<T> pop()
            {
                size_t read = m_queue->m_read.load(std::memory_order_acquire);
                if (m_localRead == read) [[unlikely]]
                {
                    return {}; // empty
                }
                T data = m_queue->m_buffer[(m_localRead++) & m_queue->m_modulo];
                return data;
            }
            explicit Reader(SPMCQueue* queue) : m_queue(queue), m_localRead(0) {}
        };
        SPMCQueue(size_t size, Allocator* alloc) :
            m_modulo(size - 1), m_buffer(size, alloc) {
            CHECK_MSG(size > 0 && (size & m_modulo) == 0, "Size must be a power of two");
        }
        bool push(T const& data)
        {
            m_buffer[(m_writeCached++) & m_modulo] = data;
            size_t read = m_read.load(std::memory_order_release);
            if (m_writeCached - read == m_buffer.size()) [[unlikely]]
                return false; // full
            m_write.store(m_writeCached, std::memory_order_release);
            m_read.store(m_writeCached, std::memory_order_release);
            return true;
        }
        Reader create_reader() { return Reader(this); }
    };
}
