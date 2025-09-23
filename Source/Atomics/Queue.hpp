#pragma once
#include <Core/Core.hpp>
#include <Core/Allocator.hpp>
#include "Atomic.hpp"
namespace Foundation::Atomics
{
    using namespace Foundation::Core;
    /**
     * @brief Atomic single-producer single-consumer FIFO ring buffer with a fixed maximum size
     * @tparam T Data type.
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
        template<typename U>
        bool push(U&& data)
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
            m_buffer[write & m_modulo] = std::forward<U>(data);
            m_write.store(write + 1, std::memory_order_release);
            return true;
        }
        /**
         * @brief _Try_ to pop data from the queue.
         * @note Caller MUST guarantee there is only _one_ concurrent thread calling pop.
         * @param out Reference to receive the popped data. This is only valid if the function returns true.
         *            The values are move-constructed from the queue.
         * @return True is successful, false if the queue is empty.
         */
        bool pop(T& out)
        {
            size_t read = m_read.load(std::memory_order_relaxed);
            // Same as above
            if (read == m_writeCached) [[unlikely]]
            {
                m_writeCached = m_write.load(std::memory_order_acquire);
                if (read == m_writeCached)
                    return false; // empty
            }
            m_read.store(read + 1, std::memory_order_release);
            out = std::move(m_buffer[read & m_modulo]);
            return true;
        }
    };
    /**
     * @brief Atomic multi-producer multi-consumer FIFO ring buffer with a fixed maximum size
     * @tparam T Data type.
     */
    template<typename T>
    class MPMCQueue
    {
        struct Data
        {
            T data{};
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
        class Writer
        {
            MPMCQueue* const queue;
        public:
            Writer(MPMCQueue* queue) : queue(queue) {}
            /**
             * @brief _Try_ to push data into the queue.
             * @note Multiple threads may call this concurrently.
             * @param data The data to push.
             * @return Whether the push was successful. Returns false if the queue is full.
             */
            template<typename U>
            bool push(U&& data) {
                size_t write = queue->m_write.load(std::memory_order_relaxed);
                while (true)
                {
                    // Stick with the current write index until we succeed
                    auto& elem = queue->m_buffer[write & queue->m_modulo];
                    size_t read_cycle = elem.read_cycle.load(std::memory_order_acquire);
                    size_t write_cycle = elem.write_cycle.load(std::memory_order_acquire);
                    if (write_cycle > read_cycle) [[unlikely]] // Still not consumed
                        return false; // full
                    size_t cycle = write >> queue->m_shift;
                    if (write_cycle == cycle) // Ready to write
                    {
                        // Bump the write index if we can, claiming the old index. Try later otherwise.
                        if (queue->m_write.compare_exchange_weak(write, write + 1, std::memory_order_relaxed))
                        {
                            elem.data = std::forward<U>(data);
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
        Writer create_writer() { return Writer(this); }
        class Reader
        {
            MPMCQueue* const queue;
        public:
            Reader(MPMCQueue* queue) : queue(queue) {}
            /**
             * @brief _Try_ to pop data from the queue.
             * @note Multiple threads may call this concurrently.
             * @param out Reference to receive the popped data. This is only valid if the function returns true.
             *            The values are move-constructed from the queue.
             * @return True is successful, false if the queue is empty.
             */
            bool pop(T& out)
            {
                size_t read = queue->m_read.load(std::memory_order_relaxed);
                while (true)
                {
                    // Same as above
                    auto& elem = queue->m_buffer[read & queue->m_modulo];
                    size_t read_cycle = elem.read_cycle.load(std::memory_order_acquire);
                    size_t write_cycle = elem.write_cycle.load(std::memory_order_relaxed);
                    if (read_cycle >= write_cycle) [[unlikely]] // Still not written
                        return false; // empty
                    size_t cycle = read >> queue->m_shift;
                    if (read_cycle == cycle) // Ready to read
                    {
                        // Same as above for reading
                        if (queue->m_read.compare_exchange_weak(read, read + 1, std::memory_order_relaxed))
                        {
                            out = std::move(elem.data);
                            elem.read_cycle.store(cycle + 1, std::memory_order_release);
                            return true;
                        }
                    } else // Old write
                        read = queue->m_read.load(std::memory_order_relaxed);
                }
            };
        };
        /**
         * @brief Create a @ref Reader for concurrent popping.
         */
        Reader create_reader() { return Reader(this); }
    };
}
