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
option(FOUNDATION_WITH_JOLT_EXAMPLES "Build Jolt Physics examples" OFF)
option(FOUNDATION_NO_EXCEPTION "Build with C++ Exception Handling disabled" ON)

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
if (MSVC)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # https://developercommunity.visualstudio.com/t/__VA_OPT__-should-be-enabled-by-std:c/10565399
        add_compile_options("/Zc:preprocessor")
    endif ()
    add_compile_options("/utf-8")
else()
    add_compile_options(-Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
endif()

if (FOUNDATION_NO_EXCEPTION)
    # No exceptions!!!
    # Needed since MSVC STL containers use this all the time.
    # Running with scissors here... though quite actively maintained by STL
    # - https://github.com/microsoft/STL/pull/5406
    # - https://github.com/microsoft/STL/issues/2216
    if (MSVC) # True for both MSVC and Clang-CL
        # Begone. /EHsc is everywhere even with Clang-CL configurations.
        set(target_vars
                CMAKE_CXX_FLAGS
                CMAKE_CXX_FLAGS_DEBUG
                CMAKE_CXX_FLAGS_RELEASE
                CMAKE_CXX_FLAGS_MINSIZEREL
                CMAKE_CXX_FLAGS_RELWITHDEBINFO
        )
        foreach(var IN LISTS target_vars)
            if(${var} MATCHES "/EHsc")
                string(REPLACE "/EHsc" "" ${var} "${${var}}")
            endif()
        endforeach()
    endif()

    if (MSVC)
        add_compile_options(/EHs-c-)
        add_compile_definitions(_HAS_EXCEPTIONS=0)
        # You might be surprised that even with enabling these flags, the final PE
        # would *still* contain Exception Unwind tables. And no - there's no way to remove them
        # from MSVC by itself.
        # Of course, you can always...
    else()
        # ...Use Clang/LLVM instead. Or Cl-Clang if you must stick with MS toolchains.
        add_compile_options(
                -fno-exceptions
                -fno-unwind-tables
                -fno-asynchronous-unwind-tables
        )
    endif()
    add_compile_definitions(FMT_EXCEPTIONS=0 FMT_USE_LOCALE=0)
endif()

# Dead-code strip and no RTTI
if (MSVC)
    add_compile_options(/GR- /Gy /Gw)
    add_link_options(/OPT:REF /OPT:ICF)
else()
    add_compile_options(-fno-rtti -ffunction-sections -fdata-sections)
    if (APPLE)
        add_link_options(-Wl,-dead_strip)
    else()
        add_link_options(-Wl,--gc-sections -s)
    endif()
endif()

# LTO
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
if (ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
else()
    message(STATUS "LTO not supported: ${ipo_msg}")
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
    if (MSVC)
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
