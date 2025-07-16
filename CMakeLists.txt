cmake_minimum_required(VERSION 3.26)
project(Foundation)
set(CMAKE_CXX_STANDARD 20)

add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

add_subdirectory(src/Core)
add_subdirectory(src/Platform)
add_subdirectory(src/Renderer)
add_subdirectory(src/Editor)

add_subdirectory(tests)
