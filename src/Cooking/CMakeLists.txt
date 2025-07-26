cmake_minimum_required(VERSION 3.26)

include(FetchContent)


FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 	  f58f558c120e9b32c217290b80bad1a0729fbb2c
)
FetchContent_MakeAvailable(stb)

FetchContent_Declare(
    fast_obj
    GIT_REPOSITORY https://github.com/thisistherk/fast_obj
    GIT_TAG d620667f10a548dee94dbc8c144bb22f79162176
)
FetchContent_MakeAvailable(fast_obj)

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
    fast_obj
    Foundation_Runtime
)
target_include_directories(Foundation_Cooking PUBLIC ..)
target_include_directories(Foundation_Cooking PRIVATE "${stb_SOURCE_DIR}" "${fast_obj_SOURCE_DIR}")
