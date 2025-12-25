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
    message(STATUS "Vulkan Valiation Layer ENABLED")
    target_compile_definitions(Foundation_RHIVulkan PUBLIC FOUNDATION_RHIVULKAN_VVL=1)
else ()
    message(STATUS "Vulkan Valiation Layer DISABLED")
    target_compile_definitions(Foundation_RHIVulkan PUBLIC FOUNDATION_RHIVULKAN_VVL=0)
endif ()

target_link_libraries(Foundation_RHIVulkan PUBLIC Foundation_RHICore Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
target_include_directories(
        Foundation_RHIVulkan
        PUBLIC
        ..
)

target_precompile_headers(
        Foundation_RHIVulkan
        PUBLIC
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc>"
)