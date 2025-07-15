#include <Core/Logging.hpp>
#include <Core/Allocator/StackAllocator.hpp>
#include <Core/Allocator/HeapAllocator.hpp>
#include <Core/Allocator/DefaultAllocator.hpp>

using namespace Foundation::Core;
constexpr size_t benchCount = 5e1;
constexpr size_t allocCount = 1e7;
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

#include <spdlog/sinks/stdout_color_sinks.h>
namespace Foundation::Platform {
    spdlog::sink_ptr GetPlatformDebugLoggingSink() {
        return std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    }
}
int main() {
    void* memory = _aligned_malloc(arenaSize, arenaSize); // 64 KiB alignment
    Allocator::Arena arena(memory, arenaSize);
	bench_many("Stack ST", [&]() {
		StackAllocatorSingleThreaded alloc(arena);
		bench_one(&alloc);
	});
	bench_many("Heap Arena", [&]() {
        HeapAllocatorSingleThreaded alloc(arena);
		bench_one(&alloc);
	});
	bench_many("Heap Default", [&]() {
        HeapAllocatorSingleThreaded alloc;
		bench_one(&alloc);
	});
}
