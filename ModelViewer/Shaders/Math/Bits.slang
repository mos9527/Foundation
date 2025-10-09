module Bits;

/* Round up for 2^k divisors */
public const uint32_t alignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

