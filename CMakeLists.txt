cmake_minimum_required(VERSION 3.26)
project(Foundation)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)

option(FOUNDATION_WITH_ASAN "Build with ASAN (Address) enabled" OFF)
option(FOUNDATION_WITH_TSAN "Build with TSAN (Thread) enabled" OFF)
option(FOUNDATION_WITH_TESTS "Build tests" ON)
option(FOUNDATION_WITH_MODELVIEWER "Build the model viewer application" ON)
option(FOUNDATION_WITH_EXAMPLES "Build examples" ON)
option(FOUNDATION_WITH_PROFILING "Build with profiling (Tracy) enabled" ON)

if (FOUNDATION_WITH_PROFILING)
    add_compile_definitions(TRACY_ENABLE=1)
else ()
    add_compile_definitions(TRACY_ENABLE=0)
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # https://developercommunity.visualstudio.com/t/__VA_OPT__-should-be-enabled-by-std:c/10565399
    add_compile_options("/Zc:preprocessor")
    add_compile_options("/utf-8")
endif()


set(MI_TRACK_ASAN FOUNDATION_WITH_ASAN CACHE BOOL "" FORCE)
if (FOUNDATION_WITH_TSAN)
    if (FOUNDATION_WITH_ASAN)
        message(FATAL_ERROR "Cannot enable both ASAN and TSAN")
    endif ()
    set(MI_TRACK_ASAN OFF CACHE BOOL "" FORCE)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        add_compile_options("/fsanitize=thread")
        add_link_options("/fsanitize=thread")
    else ()
        add_compile_options("-fsanitize=thread")
        add_link_options("-fsanitize=thread")
    endif ()
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(DERIVED_DATA_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data")
set(SHADER_BUILD_DIR "${DERIVED_DATA_DIR}/shaders")
set(ASSET_BUILD_DIR "${DERIVED_DATA_DIR}/assets")

file(MAKE_DIRECTORY ${DERIVED_DATA_DIR})
file(MAKE_DIRECTORY ${SHADER_BUILD_DIR})
file(MAKE_DIRECTORY ${ASSET_BUILD_DIR})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/Source")

add_subdirectory(Source)

if (FOUNDATION_WITH_MODELVIEWER)
    add_subdirectory(ModelViewer)
endif ()

if (FOUNDATION_WITH_EXAMPLES)
    add_subdirectory(Examples)
endif ()

if (FOUNDATION_WITH_TESTS)
    add_subdirectory(Tests)
endif ()
