cmake_minimum_required(VERSION 3.26)
file(
    GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB_RECURSE INCLUDES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})
add_library(Foundation_Core ${SOURCES} ${INCLUDES})

# Track Precompiled.inc as a real listed source so any edit forces a PCH
# rebuild even if the .ninja_deps depfile is missing/stale.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc"
    PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(Foundation_Core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc")

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
    if (ANDROID)
        set(MI_LOCAL_DYNAMIC_TLS ON CACHE BOOL "" FORCE)
    endif()
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

if (FOUNDATION_USE_PCH)
    target_precompile_headers(
        Foundation_Core
        PUBLIC
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc>"
    )
endif()

if (WIN32)
    target_link_libraries(Foundation_Core PUBLIC ws2_32 dbghelp)
elseif(ANDROID)
    target_link_libraries(Foundation_Core PUBLIC log android)
endif()

# MinGW/GCC needs libatomic for 128-bit CAS used in AtomicPool
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(Foundation_Core PUBLIC atomic)
endif()

if (FOUNDATION_WITH_ASAN)
    if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(Foundation_Core PUBLIC "/fsanitize=thread")
        target_link_options(Foundation_Core PUBLIC "/fsanitize=thread")
    else ()
        add_compile_options(Foundation_Core PUBLIC "-fsanitize=thread")
        add_link_options(Foundation_Core PUBLIC "-fsanitize=thread")
    endif ()
endif()