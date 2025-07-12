#include <Core/Core.hpp>
#include <Core/Logging.hpp>
#include <iostream>

namespace Foundation {
	namespace Core {   
		void BugCheck(const std::exception& e, const std::source_location loc) {          
            Platform::ExceptionHandler(e, Logging::GetBacktraceSink()->last_formatted());
			std::terminate();
		}
	}
}
