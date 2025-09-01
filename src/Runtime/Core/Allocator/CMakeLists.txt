cmake_minimum_required(VERSION 3.26)

file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" 
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})
add_library(Foundation_Allocator ${SOURCES} ${INCLUDES})
target_compile_definitions(
	mimalloc-static
	PRIVATE
	MI_BUILD_RELEASE
	# Always build in Release mode for mimalloc
	# e.g. OOM Allocation in Release produces nullptr,
	# but in Debug causes segfault due to asserts.
)
target_link_libraries(Foundation_Allocator PUBLIC mimalloc-static GPUOpen::VulkanMemoryAllocator)
target_include_directories(
	Foundation_Allocator
	PRIVATE
	"${mimalloc_SOURCE_DIR}/include"
)
