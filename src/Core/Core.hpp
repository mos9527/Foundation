#pragma once
namespace Foundation {
	namespace Core
	{
		constexpr const char* s_BugCheckMemoryLeak = "Memory leak detected, some allocations were not deallocated before destruction.";
		extern void BugCheck(const char* why);
	}
} // namespace Foundation
