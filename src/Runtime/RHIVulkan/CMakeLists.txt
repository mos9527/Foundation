cmake_minimum_required(VERSION 3.26)

include(FetchContent)

find_package(Vulkan REQUIRED)
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
add_library(Foundation_RHIVulkan ${SOURCES} ${INCLUDES})
target_link_libraries(Foundation_RHIVulkan PUBLIC Foundation_RHICore Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
target_include_directories(Foundation_RHIVulkan PRIVATE ..)
