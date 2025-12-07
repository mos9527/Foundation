cmake_minimum_required(VERSION 3.26)

FetchContent_MakeAvailable(stb)
FetchContent_MakeAvailable(fast_obj)
FetchContent_MakeAvailable(meshoptimizer)
FetchContent_MakeAvailable(cgltf)
FetchContent_MakeAvailable(lz4)

file(
    GLOB SOURCES
    Context.cpp
    Editor.cpp
    GPUScene.cpp
    Mesh.cpp
    Scene.cpp
    SDLMain.cpp
    Renderer.cpp
    ImGui.cpp
)

add_subdirectory(Shaders)
add_library(Editor_Assets STATIC)
target_sources(Editor_Assets PUBLIC Mesh.cpp Scene.cpp "${lz4_SOURCE_DIR}/lib/lz4.c" "${lz4_SOURCE_DIR}/lib/xxhash.c")
target_link_libraries(
    Editor_Assets PUBLIC
    Foundation_Math
    Foundation_Core
    meshoptimizer
)
target_include_directories(
    Editor_Assets PUBLIC "${stb_SOURCE_DIR}" "${fast_obj_SOURCE_DIR}" "${cgltf_SOURCE_DIR}" "${meshoptimizer_SOURCE_DIR}/demo" "${lz4_SOURCE_DIR}/lib" ..
)

add_executable(Editor)
target_sources(Editor PRIVATE ${SOURCES} ${INCLUDES} )
target_link_libraries(
        Editor PRIVATE        
        Foundation_RHIVulkan
        Foundation_RenderCore
        Foundation_RenderUtils
        ThirdParty_ImGui
        Editor_Assets
)
# https://cmake.org/cmake/help/latest/command/target_precompile_headers.html#:~:text=generator%20expressions
target_precompile_headers(
        Editor
        PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc>"
)
add_dependencies(
        Editor
        Editor_Shaders
        Examples_SampleFiles
)

add_executable(Editor_Util)
target_sources(Editor_Util PRIVATE Util.cpp)
target_link_libraries(
    Editor_Util PRIVATE
    Editor_Assets
)
