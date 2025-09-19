cmake_minimum_required(VERSION 3.26)
project(Foundation)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)
option(FOUNDATION_WITH_EXAMPLES "Build examples" ON)
option(FOUNDATION_WITH_PROFILING "Build with profiling (Tracy) enabled" ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # https://developercommunity.visualstudio.com/t/__VA_OPT__-should-be-enabled-by-std:c/10565399
    add_compile_options("/Zc:preprocessor")
    add_compile_options("/utf-8")
endif()

if (FOUNDATION_WITH_PROFILING)
    add_compile_definitions(TRACY_ENABLE=1)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/Source")

add_subdirectory(Async)
add_subdirectory(Atomic)
add_subdirectory(Bits)
add_subdirectory(Core)
add_subdirectory(Math)
add_subdirectory(Native)
add_subdirectory(RenderCore)
add_subdirectory(Rendering)
add_subdirectory(RHICore)
add_subdirectory(RHIVulkan)

add_library(Foundation INTERFACE)
target_link_libraries(Foundation INTERFACE
    Foundation_Async
    Foundation_Atomic
    Foundation_Bits
    Foundation_Core
    Foundation_Math
    Foundation_Native
    Foundation_RenderCore
    Foundation_Rendering
)
