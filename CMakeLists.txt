cmake_minimum_required(VERSION 3.26)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
project(Foundation C CXX)
if(APPLE)
    enable_language(OBJC)
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

option(FOUNDATION_WITH_ASAN "Build with ASAN (Address) enabled" OFF)
option(FOUNDATION_WITH_TSAN "Build with TSAN (Thread) enabled" OFF)
option(FOUNDATION_WITH_EXAMPLES "Build examples" ON)
option(FOUNDATION_WITH_TESTS "Build and register tests" ON)
option(FOUNDATION_WITH_JOLT_EXAMPLES "Build Jolt Physics examples" OFF)
# Android defaults: No Tracy, No VVL (you need to vendor the binary yourself..)
if (ANDROID)
    option(FOUNDATION_WITH_PROFILING "Build with profiling (Tracy) enabled" OFF)
    option(FOUNDATION_RHIVULKAN_VALIDATION_LAYER "[RHIVulkan] Build with Vulkan Validation Layer (VVL)" OFF)
else()
    option(FOUNDATION_WITH_PROFILING "Build with profiling (Tracy) enabled" ON)
    option(FOUNDATION_RHIVULKAN_VALIDATION_LAYER "[RHIVulkan] Build with Vulkan Validation Layer (VVL)" ON)
endif()
option(FOUNDATION_CORE_USES_OS_ALLOC "Use OS allocator instead of mimalloc for HeapAllocator" OFF)
option(FOUNDATION_USE_PCH "Build with target_precompile_headers (turn off to bisect stale-PCH issues)" ON)

if (FOUNDATION_WITH_PROFILING)
    add_compile_definitions(TRACY_ENABLE)    
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
else ()
    set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # https://developercommunity.visualstudio.com/t/__VA_OPT__-should-be-enabled-by-std:c/10565399
    add_compile_options("/Zc:preprocessor")
    add_compile_options("/utf-8")    
    add_compile_options(
        "$<$<CONFIG:Release>:/Gy>"
        "$<$<CONFIG:Release>:/Gw>"
    )
    add_link_options(
        "$<$<CONFIG:Release>:/INCREMENTAL:NO>"
        "$<$<CONFIG:Release>:/OPT:REF>"
        "$<$<CONFIG:Release>:/OPT:ICF>"
    )
else()
    add_compile_options("-Wall")
    # Statically link all MinGW runtime libs (libgcc, libstdc++, winpthread, atomic, etc.)
    if (WIN32)
        add_link_options(-static)
    endif()
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT FOUNDATION_IPO_SUPPORTED LANGUAGES C CXX)
if (FOUNDATION_IPO_SUPPORTED)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

set(MI_TRACK_ASAN ${FOUNDATION_WITH_ASAN} CACHE BOOL "" FORCE)
if (FOUNDATION_WITH_ASAN)
    if (FOUNDATION_WITH_TSAN)
        message(FATAL_ERROR "Cannot enable both ASAN and TSAN")
    endif ()
    set(MI_TRACK_ASAN ON CACHE BOOL "" FORCE)
endif()

if (FOUNDATION_WITH_TSAN)
    if (FOUNDATION_WITH_ASAN)
        message(FATAL_ERROR "Cannot enable both ASAN and TSAN")
    endif ()
    if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        add_compile_options("/fsanitize=thread")
        add_link_options("/fsanitize=thread")
    else ()
        add_compile_options("-fsanitize=thread")
        add_link_options("-fsanitize=thread")
    endif ()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if (ANDROID)
    set(DERIVED_DATA_DIR "${CMAKE_SOURCE_DIR}/Android/app/src/main/assets/Data")
else()
    set(DERIVED_DATA_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Data")
endif()
set(SHADER_BUILD_DIR "${DERIVED_DATA_DIR}/Shaders")
set(ASSET_BUILD_DIR "${DERIVED_DATA_DIR}/Assets")

message(STATUS "** Runtime Output Directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "** Derived Data Directory: ${DERIVED_DATA_DIR}")
message(STATUS "** Shader Build Directory: ${SHADER_BUILD_DIR}")
message(STATUS "** Asset Build Directory: ${ASSET_BUILD_DIR}")

file(MAKE_DIRECTORY ${DERIVED_DATA_DIR})
file(MAKE_DIRECTORY ${SHADER_BUILD_DIR})
file(MAKE_DIRECTORY ${ASSET_BUILD_DIR})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/Source")

include("${CMAKE_SOURCE_DIR}/DetectGitInfo.cmake")
Foundation_DetectGitInfo()
add_compile_definitions(
    FOUNDATION_GIT_COMMIT_HASH="${FOUNDATION_GIT_COMMIT_HASH}"
    FOUNDATION_GIT_COMMIT_SHORT="${FOUNDATION_GIT_COMMIT_SHORT}"
)

add_subdirectory(ThirdParty)
add_subdirectory(Source)

if (FOUNDATION_WITH_TESTS)
    enable_testing()
endif()

# NOTE: ImGui's SDL3 backend compiles on Android (SDL3 is built as a shared lib
# for the APK), but docking/keyboard/mouse IO isn't wired up yet. We build the
# library so Example_ImGui links; on-device IO is handled later.
add_subdirectory(ThirdParty/ImGui)
add_subdirectory(Editor)
if (FOUNDATION_WITH_EXAMPLES)
    add_subdirectory(Examples)
endif ()
