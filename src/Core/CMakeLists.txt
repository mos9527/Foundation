cmake_minimum_required(VERSION 3.26)

include(FetchContent)

FetchContent_Declare(
  mimalloc
  GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
  GIT_TAG        v3.1.5
)
FetchContent_MakeAvailable(mimalloc)

add_library(
	Foundation_Core 
	"Allocator/MimallocAllocator.hpp"
	"Allocator/DefaultAllocator.hpp"
	"Allocator/ArenaAllocator.cpp"
	"Allocator/Allocator.cpp" 
 "Core.cpp")

target_link_libraries(Foundation_Core PUBLIC mimalloc-static)
target_include_directories(
	Foundation_Core 
	PUBLIC 
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${mimalloc_SOURCE_DIR}/include"
)