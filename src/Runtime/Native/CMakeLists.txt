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
    Foundation_Native ${SOURCES} ${INCLUDES}    
 )

include(FetchContent)

FetchContent_Declare(
  GLFW
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        506c11ba43b901dbcc4d90449f46de67cf000af4
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(GLFW)

target_link_libraries(Foundation_Native PUBLIC Foundation_Core glfw)
target_include_directories(Foundation_Native PRIVATE .. "${GLFW_SOURCE_DIR}/include")
