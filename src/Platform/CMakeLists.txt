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

include(FetchContent)

FetchContent_Declare(
  GLFW
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        506c11ba43b901dbcc4d90449f46de67cf000af4
)
FetchContent_MakeAvailable(GLFW)

file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLATFORM_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Logging/${PLATFORM_DIR}/*.cpp"
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/${PLATFORM_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Logging/${PLATFORM_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})

add_library(
    Foundation_Platform ${SOURCES}
    "Application.cpp"
)

find_package(Vulkan REQUIRED)
target_link_libraries(Foundation_Platform PUBLIC glfw)
target_link_libraries(Foundation_Platform PUBLIC Foundation_Core)
target_link_libraries(Foundation_Platform PUBLIC Vulkan::Vulkan)
target_include_directories(Foundation_Platform PUBLIC "${GLFW_SOURCE_DIR}/include")
target_include_directories(
	Foundation_Platform 
	PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/.."
)
