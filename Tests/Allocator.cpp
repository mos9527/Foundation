#include <Core/AllocatorHeap.hpp>
#include <Core/AllocatorStack.hpp>
#include <Core/Core.hpp>

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
template<typename Func> void bench_many(const char* desc, Func&& func) {
	chrono::steady_clock::time_point start = chrono::steady_clock::now();
	for (size_t i = 0; i < benchCount; ++i) func();
    LOG_RUNTIME(Allocator, info, "{}: {} ms", desc,
        chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start).count());
}

int main() {
    void* memory = malloc(arenaSize);
    Arena arena(memory, arenaSize);
	LOG_RUNTIME(Allocator, info, "Benchmark: {} allocations of {} bytes, repeated {} times", allocCount, sizeof(int), benchCount);
	bench_many("Stack Arena", [&]() {
		AllocatorStack alloc(arena);
		bench_one(&alloc);
	});
    bench_many("Heap (mimalloc)", [&]() {
        AllocatorHeap alloc;
        bench_one(&alloc);
    });
}
