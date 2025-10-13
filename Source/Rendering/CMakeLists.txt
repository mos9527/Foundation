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
    Foundation_Rendering ${SOURCES} ${INCLUDES}
)
add_subdirectory(Shaders)
target_link_libraries(
		Foundation_Rendering
		PUBLIC
		Foundation_Atomics
		Foundation_RenderCore
		Foundation_Native
		# VirtualAllocator uses VMA
		Foundation_RHIVulkan
)
add_dependencies(Foundation_Rendering Shader_VSFullscreen)
add_dependencies(Foundation_Rendering Shader_PSCopy)
add_dependencies(Foundation_Rendering Shader_CSMipGeneration)
target_include_directories(
	Foundation_Rendering
	PUBLIC
	..
)