cmake_minimum_required(VERSION 3.26)

file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})
add_library(
    Foundation_Rendering ${SOURCES} ${INCLUDES}
)
add_subdirectory(Shaders)

target_link_libraries(Foundation_Rendering PUBLIC Foundation_Runtime)
target_include_directories(
	Foundation_Rendering 
	PRIVATE
    ..
)
