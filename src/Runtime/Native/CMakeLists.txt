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

set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  GLFW
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        3.4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(GLFW)

FetchContent_Declare(
    tinyfiledialogs
    GIT_REPOSITORY https://git.code.sf.net/p/tinyfiledialogs/code
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(tinyfiledialogs)

add_library(
    Foundation_Native ${SOURCES} ${INCLUDES}
    "${tinyfiledialogs_SOURCE_DIR}/tinyfiledialogs.c"
)

target_link_libraries(Foundation_Native PUBLIC Foundation_Core glfw)
target_link_libraries(Foundation_Native PRIVATE )
target_include_directories(
    Foundation_Native PRIVATE
    ..
    "${GLFW_SOURCE_DIR}/include"
    "${tinyfiledialogs_SOURCE_DIR}"
    )

