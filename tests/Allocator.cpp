#include <Core/Core.hpp>
#include <Core/Allocator/StackAllocator.hpp>
#include <Core/Allocator/HeapAllocator.hpp>

using namespace Foundation::Core;
constexpr size_t benchCount = 6e1;
constexpr size_t allocCount = 1e5;
constexpr size_t arenaSize = 512 * 1024 * 1024; // 512 MB

#include <vector>
#include <chrono>
using namespace std;

auto bench_one(Allocator* allocator) {
    StlAllocator<int> alloc(allocator);
    vector<int, StlAllocator<int>> vec(alloc);
    for (size_t i = 0; i < allocCount; ++i) {
        vec.push_back(i);
    }
}
auto bench_one_stl() {
    vector<int> vec;
    for (size_t i = 0; i < allocCount; ++i) {
        vec.push_back(i);
    }
}
template<typename Func> void bench_many(const char* desc, Func&& func) {
	chrono::steady_clock::time_point start = chrono::steady_clock::now();
	for (size_t i = 0; i < benchCount; ++i) func();
    LOG_RUNTIME(Allocator, info, "{}: {} ms", desc,
        chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count());
}

int main() {
    void* memory = aligned_alloc(arenaSize, arenaSize); // 64 KiB alignment
    Arena arena(memory, arenaSize);
	LOG_RUNTIME(Allocator, info, "Benchmark: {} allocations of {} bytes, repeated {} times", allocCount, sizeof(int), benchCount);
	bench_many("Stack Arena", [&]() {
		StackAllocator alloc(arena);
		bench_one(&alloc);
	});
    bench_many("Heap (tracking)", [&]() {
        HeapAllocator<true> alloc;
        bench_one(&alloc);
    });
    bench_many("Heap (non-tracking)", [&]() {
        HeapAllocator<false> alloc;
        bench_one(&alloc);
    });
	bench_many("OS Default (STL)", [&]() {
	    bench_one_stl();
	});
}
