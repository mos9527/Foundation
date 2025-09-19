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
    Foundation_RenderCore ${SOURCES} ${INCLUDES}
)
target_link_libraries(Foundation_RenderCore PUBLIC Foundation_RHICore Foundation_RHIVulkan Foundation_Runtime)
target_include_directories(
	Foundation_RenderCore
	PUBLIC
	..
)