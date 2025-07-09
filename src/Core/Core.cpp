#include <Core/Core.hpp>
#include <iostream>

namespace Foundation {
	namespace Core {
		// Halt the program and print a message to stderr.
		void BugCheck(const char* why) {
			std::cerr << "BugCheck: " << why << std::endl;
			std::terminate();
		}
	}
}
