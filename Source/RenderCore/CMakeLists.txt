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

FetchContent_MakeAvailable(SPIRV-Headers)
FetchContent_MakeAvailable(SDL3)

add_library(
    Foundation_RenderCore ${SOURCES} ${INCLUDES}
)

target_link_libraries(
	Foundation_RenderCore PUBLIC
	Foundation_Core
	Foundation_Async
	Foundation_Atomics
	Foundation_RHICore
	SPIRV-Headers::SPIRV-Headers
	SDL3::SDL3-static
)
target_include_directories(
	Foundation_RenderCore
	PUBLIC
	..
)
target_precompile_headers(
	Foundation_RenderCore
	PUBLIC
	Precompiled.inc
)