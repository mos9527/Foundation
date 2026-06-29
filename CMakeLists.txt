cmake_minimum_required(VERSION 3.26)
project(Foundation C CXX)
if(APPLE)
    enable_language(OBJC)
endif()
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

option(FOUNDATION_WITH_ASAN "Build with ASAN (Address) enabled" OFF)
option(FOUNDATION_WITH_TSAN "Build with TSAN (Thread) enabled" OFF)
option(FOUNDATION_WITH_MODELVIEWER "Build the model viewer application" ON)
option(FOUNDATION_WITH_EXAMPLES "Build examples" ON)
option(FOUNDATION_WITH_PROFILING "Build with profiling (Tracy) enabled" ON)
option(FOUNDATION_CORE_USES_OS_ALLOC "Use OS allocator instead of mimalloc for HeapAllocator" OFF)
option(FOUNDATION_RHIVULKAN_VALIDATION_LAYER "[RHIVulkan] Build with Vulkan Validation Layer (VVL)" ON)
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
else()
    add_compile_options("-Wall")
    # Statically link all MinGW runtime libs (libgcc, libstdc++, winpthread, atomic, etc.)
    if (WIN32)
        add_link_options(-static)
    endif()
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
set(DERIVED_DATA_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/Data")
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

find_package(Git)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_COMMIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    add_compile_definitions(FOUNDATION_GIT_COMMIT_HASH="${GIT_COMMIT_HASH}")
else()
    add_compile_definitions(FOUNDATION_GIT_COMMIT_HASH="unknown")
endif()

add_subdirectory(ThirdParty)
add_subdirectory(Source)

add_subdirectory(ThirdParty/ImGui)

add_subdirectory(Editor)
if (FOUNDATION_WITH_EXAMPLES)
    add_subdirectory(Examples)
endif ()
