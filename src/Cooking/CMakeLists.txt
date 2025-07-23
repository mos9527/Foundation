cmake_minimum_required(VERSION 3.26)

include(FetchContent)

FetchContent_Declare(
  meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG        e8879f358d9dccf3038dd194e99d2749347da1dd
)
FetchContent_MakeAvailable(meshoptimizer)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 	  f58f558c120e9b32c217290b80bad1a0729fbb2c
)
FetchContent_MakeAvailable(stb)

file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" 
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})
add_library(Foundation_Cooking ${SOURCES} ${INCLUDES})

target_link_libraries(Foundation_Cooking
    PUBLIC
    meshoptimizer
    Foundation_Runtime
)
target_include_directories(Foundation_Cooking PUBLIC ..)
target_include_directories(Foundation_Cooking PRIVATE "${stb_SOURCE_DIR}")
