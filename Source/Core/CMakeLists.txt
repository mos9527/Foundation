cmake_minimum_required(VERSION 3.26)
file(
        GLOB_RECURSE SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
        GLOB_RECURSE INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})
add_library(Foundation_Core ${SOURCES} ${INCLUDES})

include(FetchContent)

FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 11.2.0
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.15.3
        GIT_SHALLOW TRUE
)
set(SPDLOG_FMT_EXTERNAL ON)
FetchContent_MakeAvailable(spdlog)

set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG v3.1.5
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(mimalloc)

target_link_libraries(Foundation_Core PUBLIC fmt::fmt spdlog::spdlog)
option(FOUNDATION_CORE_USES_OS_ALLOC "Use OS allocator instead of mimalloc for HeapAllocator" OFF)
if (FOUNDATION_CORE_USES_OS_ALLOC)
    target_compile_definitions(Foundation_Core PUBLIC "FOUNDATION_CORE_USES_OS_ALLOC=1")
else ()    
    target_compile_definitions(Foundation_Core PUBLIC "FOUNDATION_CORE_USES_OS_ALLOC=0")
    target_link_libraries(Foundation_Core PUBLIC mimalloc-static)
endif()

FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.12.2
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tracy)



target_include_directories(
        Foundation_Core
        PUBLIC
        ..
        ${tracy_SOURCE_DIR}/public
        ${mimalloc_SOURCE_DIR}/include
)