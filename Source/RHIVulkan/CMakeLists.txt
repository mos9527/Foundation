cmake_minimum_required(VERSION 3.26)

file(
        GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
        GLOB_RECURSE INCLUDES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

find_package(Vulkan REQUIRED)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

add_library(Foundation_RHIVulkan ${SOURCES} ${INCLUDES})

set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc"
        PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(Foundation_RHIVulkan PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc")

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

if (FOUNDATION_USE_PCH)
    target_precompile_headers(
            Foundation_RHIVulkan
            PUBLIC
            "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc>"
    )
endif()