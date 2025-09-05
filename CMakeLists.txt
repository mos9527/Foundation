cmake_minimum_required(VERSION 3.26)
project(Foundation)
set(CMAKE_CXX_STANDARD 20)

add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(DERIVED_DATA_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/data")
set(SHADER_BUILD_DIR "${DERIVED_DATA_DIR}/shaders")
file(MAKE_DIRECTORY ${DERIVED_DATA_DIR})
file(MAKE_DIRECTORY ${SHADER_BUILD_DIR})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/src/CMake")

add_subdirectory(src/Examples)
add_subdirectory(src/ModelViewer)
add_subdirectory(src/Rendering)
add_subdirectory(src/Runtime)

add_subdirectory(tests)
