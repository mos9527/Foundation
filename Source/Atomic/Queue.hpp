#pragma once
#include <Core/Core.hpp>
#include <Core/Allocator.hpp>
#include "Atomic.hpp"
/**
 * @brief Lock-free atomic implementations of data structures.
 *
 * References:
 * - Dmitry Vyukov's blog on lock-free algorithms and data structures
 *   - https://www.1024cores.net/home/lock-free-algorithms/introduction
 * - CppCon 2014: Herb Sutter "Lock-Free Programming (or, Juggling Razor Blades), Part I"
 *   - https://www.youtube.com/watch?v=c1gO9aB9nbs
 * - Single Producer Single Consumer Lock-free FIFO From the Ground Up - Charles Frasch - CppCon 2023
 *   - https://www.youtube.com/watch?v=K3P_Lmq6pw0
 * - Djordje Nedic's lockfree
 *   - https://github.com/DNedic/lockfree
 * - std::memory_order on cppreference
 *   - https://en.cppreference.com/w/cpp/atomic/memory_order.html
 */
namespace Foundation::Atomic
{
    using namespace Foundation::Core;
    /**
     * @brief Atomic single-producer single-consumer ring buffer with a fixed maximum size
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
            // We don't need to update the read head everytime. Only when we think we're full.
            if (write - m_readCached == m_buffer.size()) [[unlikely]]
            {
                // Wrapped. Next cycle could begin only when there's enough space
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
         * @return Data pointer if successful, nullptr if the queue is empty.
         */
        T* pop()
        {
            size_t read = m_read.load(std::memory_order_relaxed);
            // Same as above
            if (read == m_writeCached) [[unlikely]]
            {
                m_writeCached = m_write.load(std::memory_order_acquire);
                if (read == m_writeCached)
                    return nullptr; // empty
            }
            m_read.store(read + 1, std::memory_order_release);
            return &m_buffer[read & m_modulo];
        }
    };
    /**
     * @brief Atomic multi-producer multi-consumer ring buffer with a fixed maximum size
     */
    template<typename T>
    class MPMCQueue
    {
        struct Data
        {
            T data;
            Atomic<size_t> write_cycle{} /* when to write */ , read_cycle{} /* when to read */;
        };
        const size_t m_modulo, m_shift;
        Vector<Data> m_buffer;
        Atomic<size_t> m_read{}, m_write{};
        size_t m_writeCached{};
    public:
        MPMCQueue(size_t size, Allocator* alloc) :
            m_modulo(size - 1), m_shift(std::countr_zero(size)), m_buffer(size, alloc) {
            CHECK_MSG(size > 0 && (size & m_modulo) == 0, "Size must be a power of two");
        }
        struct Writer
        {
            MPMCQueue* queue;
            size_t readCached;
            /**
             * @brief _Try_ to push data into the queue.
             * @note Multiple threads may call this concurrently.
             * @param data The data to push.
             * @return Whether the push was successful. Returns false if the queue is full.
             */
            bool push(T const& data) {
                size_t write = queue->m_write.load(std::memory_order_relaxed);
                while (true)
                {
                    // Stick with the current write index until we succeed
                    auto& elem = queue->m_buffer[write & queue->m_modulo];
                    size_t write_cycle = elem.write_cycle.load(std::memory_order_acquire);
                    if (write_cycle > readCached) [[unlikely]] // Still not consumed
                    {
                        readCached = elem.read_cycle.load(std::memory_order_relaxed);
                        if (write_cycle > readCached)
                            return false; // full
                    }
                    size_t cycle = write >> queue->m_shift;
                    if (write_cycle == cycle) // Ready to write
                    {
                        // Bump the write index if we can, claiming the new index. Try later otherwise.
                        if (queue->m_write.compare_exchange_weak(write, write + 1, std::memory_order_relaxed))
                        {
                            elem.data = data;
                            elem.write_cycle.store(cycle + 1, std::memory_order_release);
                            return true;
                        }
                    } else // Not our turn yet? So we must be an old write. Update and try again.
                        write = queue->m_write.load(std::memory_order_relaxed);
                }
            }
        };
        /**
         * @brief Create a @ref Writer for concurrent pushing.
         */
        Writer create_writer() { return Writer{ this, 0 }; }
        struct Reader
        {
            MPMCQueue* queue;
            size_t writeCached;
            /**
             * @brief _Try_ to pop data from the queue.
             * @note Multiple threads may call this concurrently.*
             * @return Data pointer if successful, nullptr if the queue is empty.
             */
            T* pop()
            {
                size_t read = queue->m_read.load(std::memory_order_relaxed);
                while (true)
                {
                    // Same as above
                    auto& elem = queue->m_buffer[read & queue->m_modulo];
                    size_t read_cycle = elem.read_cycle.load(std::memory_order_acquire);
                    if (read_cycle >= writeCached) [[unlikely]] // Still not written
                    {
                        writeCached = elem.write_cycle.load(std::memory_order_relaxed);
                        if (read_cycle >= writeCached)
                            return nullptr; // empty
                    }
                    size_t cycle = read >> queue->m_shift;
                    if (read_cycle == cycle) // Ready to read
                    {
                        // Same as above for reading
                        if (queue->m_read.compare_exchange_weak(read, read + 1, std::memory_order_relaxed))
                        {
                            T* result = &elem.data;
                            elem.read_cycle.store(cycle + 1, std::memory_order_release);
                            return result;
                        }
                    } else // Old write
                        read = queue->m_read.load(std::memory_order_relaxed);
                }
            };
        };
        /**
         * @brief Create a @ref Reader for concurrent popping.
         */
        Reader create_reader() { return Reader{ this, 0 }; }
    };
}
