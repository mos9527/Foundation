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

FetchContent_Declare(
  SPIRV-Headers
  GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers
  GIT_TAG        vulkan-sdk-1.4.321.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SPIRV-Headers)

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
)
target_include_directories(
	Foundation_RenderCore
	PUBLIC
	..
)
