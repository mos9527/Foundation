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
  GIT_TAG        3.4
)
FetchContent_MakeAvailable(GLFW)

FetchContent_Declare(
    tinyfiledialogs
    GIT_REPOSITORY https://git.code.sf.net/p/tinyfiledialogs/code
    GIT_TAG 96c52c82399dbfce0ad6bf975dedd1ce522b3951
)
FetchContent_MakeAvailable(tinyfiledialogs)

target_link_libraries(Foundation_Native PUBLIC Foundation_Core glfw)
target_include_directories(Foundation_Native PRIVATE .. "${GLFW_SOURCE_DIR}/include")
