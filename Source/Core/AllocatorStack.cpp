namespace Foundation::Core {
    pointer AllocatorStack::Allocate(size_type size, size_type alignment)
    {
        if (size == 0)
            return nullptr;        
        size_t current = mCurrent.load(std::memory_order_relaxed);
        while (true)
        {
            auto aligned = AlignUp(current, alignment);
            auto next = aligned + size;

            CHECK(next <= mEnd);
            
            if (mCurrent.compare_exchange_weak(current, next, std::memory_order_release,
                                                std::memory_order_relaxed))
            {
                // Bump OK. Good to go!
                return reinterpret_cast<pointer>(aligned);
            }
            current = mCurrent.load(std::memory_order_relaxed);
        }
    };
}
