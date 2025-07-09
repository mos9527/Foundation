cmake_minimum_required(VERSION 3.26)

if (WIN32)
    set(PLATFORM_DIR "Windows")
elseif (APPLE)
    set(PLATFORM_DIR "macOS")
elseif (UNIX)
    set(PLATFORM_DIR "Linux")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLATFORM_DIR}/*.cpp" 
    "${CMAKE_CURRENT_SOURCE_DIR}/Render/*.cpp"
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLATFORM_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Render/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})

add_library(Foundation_Platform ${SOURCES} ${INCLUDES})

