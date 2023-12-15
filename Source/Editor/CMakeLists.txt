cmake_minimum_required(VERSION 3.26)

file(GLOB_RECURSE SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB_RECURSE INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${INCLUDES})

add_compile_definitions(SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Shaders")

add_executable(Editor "Main.cpp" ${SOURCES} ${INCLUDES})
target_include_directories(Editor PRIVATE Runtime)
target_precompile_headers(Editor REUSE_FROM PCH)
target_link_libraries(Editor PUBLIC PCH)
target_link_libraries(Editor PUBLIC Runtime)

# Additional DLLs
add_custom_command(
        TARGET Editor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
        "${DEPENDS}/dxc/dxcompiler.dll"
        "${DEPENDS}/dxc/dxil.dll"
        "${DEPENDS}/winpixeventruntime/WinPixEventRuntime.dll"

        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
)