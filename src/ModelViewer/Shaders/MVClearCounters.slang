#include "Common.h"

RWStructuredBuffer<Atomic<uint>> counter;
[shader("compute")]
[numthreads(1,1,1)]
void resetCounter(){
    counter[0].store(0);
}
