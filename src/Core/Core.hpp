#pragma once
#include <vector>
#include <stdexcept>
#include <source_location>
namespace Foundation {
    namespace Platform {
        extern void ExceptionHandler(const std::exception& e, std::vector<std::string> backtrace);
    }
	namespace Core
	{
		constexpr const char* sBugCheckMemoryLeak = "Memory leak detected, some allocations were not deallocated before destruction.";        
		extern void BugCheck(const std::exception& e, const std::source_location loc = std::source_location::current());
	}
} // namespace Foundation

#ifdef _DEBUG
#define DEBUG_RUN(expr) expr;   
#else
#define DEBUG_RUN(expr) (void)0
#endif
