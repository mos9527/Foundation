#include <Core/Logging.hpp>
#include <Core/Allocator/ArenaAllocator.hpp>
#include <Core/Allocator/DefaultAllocator.hpp>
#include <Core/Allocator/MimallocAllocator.hpp>

using namespace Foundation::Core;
constexpr size_t benchCount = 1e2;
constexpr size_t allocCount = 1e6;
constexpr size_t arenaSize = 128 * 1024 * 1024; // 128 MB

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
	chrono::steady_clock::time_point end = chrono::steady_clock::now();
	auto duration = chrono::duration_cast<chrono::milliseconds>(end - start);
    LOG_RUNTIME(Allocator, info, "{}: {} ms", desc, duration.count());
}

#include <spdlog/sinks/stdout_color_sinks.h>
int main() {
    LOG_GET_GLOBAL_SINK()->add_sink(        
        std::make_shared<spdlog::sinks::stderr_color_sink_mt>()
    );
	vector<char> arena(arenaSize);
	bench_many("Arena", [&]() {
		ArenaAllocator arenaAllocator(arena.data(), arena.size());
		bench_one(&arenaAllocator);
	});
	bench_many("Mimalloc", [&]() {
		MimallocAllocator mimallocAllocator;
		bench_one(&mimallocAllocator);
	});
	bench_many("Malloc", [&]() {
		DefaultAllocator defaultAllocator;
		bench_one(&defaultAllocator);
	});
	bench_many("STL", [&]() {	
		vector<int> vec;
		for (size_t i = 0; i < allocCount; ++i) {
			vec.push_back(i);
		}
	});
}
