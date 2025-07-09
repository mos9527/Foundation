cmake_minimum_required(VERSION 3.26)

include(FetchContent)

FetchContent_Declare(
  VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG        v3.3.0
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})

add_library(
    Foundation_Renderer ${SOURCES} ${INCLUDES}
)

find_package(Vulkan REQUIRED)
target_link_libraries(Foundation_Renderer PUBLIC Foundation_Core)
target_link_libraries(Foundation_Renderer PRIVATE GPUOpen::VulkanMemoryAllocator)
target_link_libraries(Foundation_Renderer PRIVATE Vulkan::Vulkan)
target_include_directories(
	Foundation_Platform 
	PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/.."
)
