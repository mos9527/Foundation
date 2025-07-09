cmake_minimum_required(VERSION 3.26)

include(FetchContent)

FetchContent_Declare(
  mimalloc
  GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
  GIT_TAG        v3.1.5
)
FetchContent_MakeAvailable(mimalloc)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        11.2.0
)
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.3
)
set(SPDLOG_FMT_EXTERNAL ON)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

add_library(
	Foundation_Core 
	"Allocator/MimallocAllocator.hpp"
	"Allocator/DefaultAllocator.hpp"
	"Allocator/Allocator.hpp" 
	"Allocator/ArenaAllocator.cpp"
	"Logging.hpp"
	"Logging.cpp"
	"Core.cpp"
	"Math.hpp"
)

target_link_libraries(Foundation_Core PUBLIC glm mimalloc-static fmt::fmt spdlog)
target_include_directories(
	Foundation_Core 
	PUBLIC
	"${spdlog_SOURCE_DIR}/include"	
	"${mimalloc_SOURCE_DIR}/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/.."
)
