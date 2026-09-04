cmake_minimum_required(VERSION 3.26)

file(
        GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
        GLOB_RECURSE INCLUDES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

# Android NDK provides Vulkan headers in the sysroot and libvulkan.so on-device.
# find_package may not locate them through the NDK toolchain, so provide a fallback.
if (ANDROID)
    # The NDK sysroot provides vulkan.h and libvulkan.so, so find_package(Vulkan) might succeed.
    # However, it never provides vulkan.hpp. Therefore, we must ALWAYS fetch Vulkan-Headers on Android.
    include(FetchContent)
    FetchContent_Declare(
        VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.4.350
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(VulkanHeaders)

    find_package(Vulkan QUIET)
    if (NOT Vulkan_FOUND)
        add_library(Vulkan::Vulkan SHARED IMPORTED)
        set_target_properties(Vulkan::Vulkan PROPERTIES
            IMPORTED_LOCATION "${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/libvulkan.so"
        )
    endif()
else()
    find_package(Vulkan REQUIRED)
endif()
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

FetchContent_MakeAvailable(SDL3)
target_link_libraries(Foundation_RHIVulkan PUBLIC Foundation_RHICore Vulkan::Vulkan GPUOpen::VulkanMemoryAllocator)
if (ANDROID)
    target_link_libraries(Foundation_RHIVulkan PUBLIC SDL3::SDL3-shared Vulkan::Headers android)
else()
    target_link_libraries(Foundation_RHIVulkan PUBLIC SDL3::SDL3-static)
endif()
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