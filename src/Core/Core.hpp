#pragma once
namespace Foundation {
	namespace Core
	{
		constexpr const char* s_BugCheckMemoryLeak = "Memory leak detected, some allocations were not deallocated before destruction.";
		extern void BugCheck(const char* why);
	}
} // namespace Foundation

#ifdef _DEBUG
#define DEBUG_RUN(expr) expr;   
#else
#define DEBUG_RUN(expr) (void)0
#endif
