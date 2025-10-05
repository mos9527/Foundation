cmake_minimum_required(VERSION 3.26)

file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" 
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

find_package(Vulkan REQUIRED)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

add_library(Foundation_RHIVulkan ${SOURCES} ${INCLUDES})

if (FOUNDATION_RHIVULKAN_VALIDATION_LAYER)
    target_compile_definitions(Foundation_RHIVulkan PUBLIC FOUNDATION_RHIVULKAN_VVL=1)
else()
    target_compile_definitions(Foundation_RHIVulkan PUBLIC FOUNDATION_RHIVULKAN_VVL=0)
endif()

target_link_libraries(Foundation_RHIVulkan PUBLIC Foundation_Native Foundation_RHICore Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
target_include_directories(
    Foundation_RHIVulkan
    PUBLIC
    ..
)
