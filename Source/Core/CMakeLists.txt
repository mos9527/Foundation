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
FetchContent_MakeAvailable(fmt)
FetchContent_MakeAvailable(tracy)

target_link_libraries(Foundation_Core PUBLIC fmt::fmt)

if (FOUNDATION_CORE_USES_OS_ALLOC)
    target_compile_definitions(Foundation_Core PUBLIC "FOUNDATION_CORE_USES_OS_ALLOC=1")
else ()    
    set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
    set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(mimalloc)

    target_compile_definitions(Foundation_Core PUBLIC "FOUNDATION_CORE_USES_OS_ALLOC=0")
    target_link_libraries(Foundation_Core PUBLIC mimalloc-static)
endif()

target_include_directories(
        Foundation_Core
        PUBLIC
        ..
        ${tracy_SOURCE_DIR}/public
        ${mimalloc_SOURCE_DIR}/include
)

target_precompile_headers(
    Foundation_Core
    PUBLIC
    Precompiled.inc
)
