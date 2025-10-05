cmake_minimum_required(VERSION 3.26)

file(
    GLOB SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})

include(FetchContent)
# GLFW
# HiDPi issues remain at large with Wayland backend in GLFW. Disabled for now.
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)
if (UNIX AND NOT APPLE)
    # https://github.com/glfw/glfw/issues/2495
    # XXX: This is a bit of a hack.
    # With Unity builds _GNU_SOURCE is defined too late for some reason
    target_compile_definitions(glfw PUBLIC -D_GNU_SOURCE)
endif()
FetchContent_MakeAvailable(tinyfiledialogs)
add_library(
    Foundation_Native ${SOURCES} ${INCLUDES}
    "${tinyfiledialogs_SOURCE_DIR}/tinyfiledialogs.c"
)

target_link_libraries(Foundation_Native PUBLIC Foundation_Core glfw)
target_include_directories(
    Foundation_Native PRIVATE
    ..
    "${GLFW_SOURCE_DIR}/include"
    "${tinyfiledialogs_SOURCE_DIR}"
)
target_include_directories(
    Foundation_Native PUBLIC
    ..
)
