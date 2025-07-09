cmake_minimum_required(VERSION 3.26)
project(Foundation)

add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

add_subdirectory(src/Core)
add_subdirectory(src/Platform)

add_compile_definitions(FOUNDATION_RENDERER_VERSION_MAJOR=0)
add_compile_definitions(FOUNDATION_RENDERER_VERSION_MINOR=0)
add_compile_definitions(FOUNDATION_RENDERER_VERSION_PATCH=1)
add_subdirectory(src/Renderer)

add_compile_definitions(FOUNDATION_EDITOR_VERSION_MAJOR=0)
add_compile_definitions(FOUNDATION_EDITOR_VERSION_MINOR=0)
add_compile_definitions(FOUNDATION_EDITOR_VERSION_PATCH=1)
add_subdirectory(src/Editor)

add_subdirectory(tests)
