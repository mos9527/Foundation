#pragma once
#include <Core/Core.hpp>
#include <Core/Allocator.hpp>
#include "Atomic.hpp"
namespace Foundation::Atomics
{
    using namespace Foundation::Core;
    /**
     * @brief Atomic, bounded single-producer single-consumer FIFO ring buffer with a fixed maximum size
     * @tparam T Data type.
     */
    template <typename T>
    class SPSCQueue
    {
        const size_t mModulo;
        Vector<T> mBuffer;
        Atomic<size_t> mRead{}, mWrite{};
        // Only used in reader thread
        size_t mReadCached{};
        // Only used in writer thread
        size_t mWriteCached{};
    public:
        /**
         * @brief Construct the SPSC Queue.
         * @param size Bounded size of the queue. Must be a power of two.
         * @param alloc Allocator to use for internal storage.
         */
        SPSCQueue(size_t size, Allocator* alloc) :
            mModulo(size - 1), mBuffer(size, alloc) {
            CHECK_MSG(size > 0 && (size & mModulo) == 0, "Size must be a power of two");
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
            size_t write = mWrite.load(std::memory_order_relaxed);
            // Amortize atomic reads
            // We don't need to update the read head everytime. Only when we think we're full.
            if (write - mReadCached == mBuffer.size()) [[unlikely]]
            {
                // Wrapped. Next cycle could begin only when there's enough space
                mReadCached = mRead.load(std::memory_order_acquire);
                if (write - mReadCached == mBuffer.size())
                    return false; // full
            }
            mBuffer[write & mModulo] = std::forward<U>(data);
            mWrite.store(write + 1, std::memory_order_release);
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
            size_t read = mRead.load(std::memory_order_relaxed);
            // Same as above
            if (read == mWriteCached) [[unlikely]]
            {
                mWriteCached = mWrite.load(std::memory_order_acquire);
                if (read == mWriteCached)
                    return false; // empty
            }
            mRead.store(read + 1, std::memory_order_release);
            out = std::move(mBuffer[read & mModulo]);
            return true;
        }
    };
    /**
     * @brief Atomic, bounded multi-producer multi-consumer FIFO ring buffer with a fixed maximum size
     * @tparam T Data type.
     */
    template<typename T>
    class MPMCQueue
    {
        struct Data
        {
            T data{};
            Atomic<size_t> writeCycle{} /* when to write */ , readCycle{} /* when to read */;
        };
        const size_t mModulo, mShift;
        Vector<Data> mBuffer;
        Atomic<size_t> mRead{}, mWrite{};
        size_t mWriteCached{};
    public:
        MPMCQueue(size_t size, Allocator* alloc) :
            mModulo(size - 1), mShift(std::countr_zero(size)), mBuffer(size, alloc) {
            CHECK_MSG(size > 0 && (size & mModulo) == 0, "Size must be a power of two");
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
                size_t write = queue->mWrite.load(std::memory_order_relaxed);
                while (true)
                {
                    // Stick with the current write index until we succeed
                    auto& elem = queue->mBuffer[write & queue->mModulo];
                    size_t read_cycle = elem.readCycle.load(std::memory_order_acquire);
                    size_t write_cycle = elem.writeCycle.load(std::memory_order_acquire);
                    if (write_cycle > read_cycle) [[unlikely]] // Still not consumed
                        return false; // full
                    size_t cycle = write >> queue->mShift;
                    if (write_cycle == cycle) // Ready to write
                    {
                        // Bump the write index if we can, claiming the old index. Try later otherwise.
                        if (queue->mWrite.compare_exchange_weak(write, write + 1, std::memory_order_relaxed))
                        {
                            elem.data = std::forward<U>(data);
                            elem.writeCycle.store(cycle + 1, std::memory_order_release);
                            return true;
                        }
                    } else // Not our turn yet? So we must be an old write. Update and try again.
                        write = queue->mWrite.load(std::memory_order_relaxed);
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
                size_t read = queue->mRead.load(std::memory_order_relaxed);
                while (true)
                {
                    // Same as above
                    auto& elem = queue->mBuffer[read & queue->mModulo];
                    size_t read_cycle = elem.readCycle.load(std::memory_order_acquire);
                    size_t write_cycle = elem.writeCycle.load(std::memory_order_relaxed);
                    if (read_cycle >= write_cycle) [[unlikely]] // Still not written
                        return false; // empty
                    size_t cycle = read >> queue->mShift;
                    if (read_cycle == cycle) // Ready to read
                    {
                        // Same as above for reading
                        if (queue->mRead.compare_exchange_weak(read, read + 1, std::memory_order_relaxed))
                        {
                            out = std::move(elem.data);
                            elem.readCycle.store(cycle + 1, std::memory_order_release);
                            return true;
                        }
                    } else // Old write
                        read = queue->mRead.load(std::memory_order_relaxed);
                }
            };
        };
        /**
         * @brief Create a @ref Reader for concurrent popping.
         */
        Reader create_reader() { return Reader(this); }
    };
}
