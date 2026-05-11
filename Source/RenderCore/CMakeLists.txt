cmake_minimum_required(VERSION 3.26)

file(
    GLOB SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB INCLUDES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})

FetchContent_MakeAvailable(SPIRV-Headers)

add_library(
    Foundation_RenderCore ${SOURCES} ${INCLUDES}
)

set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc"
    PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(Foundation_RenderCore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc")

target_link_libraries(
	Foundation_RenderCore PUBLIC
	Foundation_Core
	Foundation_RHICore
	SPIRV-Headers::SPIRV-Headers
)
target_include_directories(
	Foundation_RenderCore
	PUBLIC
	..
)
if (FOUNDATION_USE_PCH)
	target_precompile_headers(
		Foundation_RenderCore
		PUBLIC
		"$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc>"
	)
endif()
if (FOUNDATION_WITH_ASAN)
	if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
		target_compile_options(Foundation_RenderCore PUBLIC "/fsanitize=thread")
		target_link_options(Foundation_RenderCore PUBLIC "/fsanitize=thread")
	else ()
		add_compile_options(Foundation_RenderCore PUBLIC "-fsanitize=thread")
		add_link_options(Foundation_RenderCore PUBLIC "-fsanitize=thread")
	endif ()
endif()