cmake_minimum_required(VERSION 3.26)


file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" 
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

include(FetchContent)

find_package(Vulkan REQUIRED)

FetchContent_Declare(
  VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG        v3.3.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# XXX: Probably not required
# On Windows Vulkan SDK comes with these but I couldn't find them on Linux
FetchContent_Declare(
  SPIRV-Headers
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers
  GIT_TAG        vulkan-sdk-1.4.321.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SPIRV-Headers)
add_library(Foundation_RHIVulkan ${SOURCES} ${INCLUDES})
target_link_libraries(Foundation_RHIVulkan PUBLIC Foundation_Native Foundation_RHICore Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator SPIRV-Headers::SPIRV-Headers)
target_include_directories(
    Foundation_RHIVulkan
    PUBLIC
    ..
)