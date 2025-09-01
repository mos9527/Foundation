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

add_library(
    Foundation_Platform ${SOURCES} ${INCLUDES}    
 )

target_link_libraries(Foundation_Platform PUBLIC fmt::fmt spdlog::spdlog glfw)
target_include_directories(Foundation_Platform PRIVATE "${GLFW_SOURCE_DIR}/include")

if (WIN32)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/Windows")
    target_link_libraries(Foundation_Platform PRIVATE Foundation_Platform_WIN32)
elseif (APPLE)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/Apple")
    target_link_libraries(Foundation_Platform PRIVATE Foundation_Platform_APPLE)
elseif (UNIX)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/Linux")
    target_link_libraries(Foundation_Platform PRIVATE Foundation_Platform_LINUX)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()
