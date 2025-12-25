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
    Foundation_RenderUtils ${SOURCES} ${INCLUDES}
)
add_subdirectory(Shaders)
target_link_libraries(
		Foundation_RenderUtils
		PUBLIC		
		Foundation_RenderCore		
)
add_dependencies(Foundation_RenderUtils Shader_VSFullscreen)
add_dependencies(Foundation_RenderUtils Shader_PSCopy)
add_dependencies(Foundation_RenderUtils Shader_CSMipGeneration Shader_CSMipGenerationSinglePass)
add_dependencies(Foundation_RenderUtils Shader_CSDebugText)
add_dependencies(Foundation_RenderUtils Shader_CSClearBuffer)
target_include_directories(
	Foundation_RenderUtils
	PUBLIC
	..
)