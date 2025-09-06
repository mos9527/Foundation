#pragma once
#include <chrono>
namespace Foundation {    
    inline double getEpochTime() {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 1e9;
    }
    inline size_t getPerformanceCounter() {
        using namespace std::chrono;
        return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    }
}
