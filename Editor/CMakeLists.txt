cmake_minimum_required(VERSION 3.26)

FetchContent_MakeAvailable(stb)
FetchContent_MakeAvailable(fast_obj)
FetchContent_MakeAvailable(meshoptimizer)
FetchContent_MakeAvailable(cgltf)
FetchContent_MakeAvailable(lz4)
FetchContent_MakeAvailable(bc7enc)
FetchContent_MakeAvailable(argh)

add_subdirectory(Shaders)
add_library(Editor_Assets STATIC)
target_sources(Editor_Assets PUBLIC
        Mesh.cpp Scene.cpp Texture.cpp
        "${lz4_SOURCE_DIR}/lib/lz4.c"
        "${lz4_SOURCE_DIR}/lib/xxhash.c"
        "${bc7enc_SOURCE_DIR}/bc7enc.c"
)
target_link_libraries(
        Editor_Assets PUBLIC
        ThirdParty_STB
        Foundation_Math
        Foundation_Core
        meshoptimizer
)
target_include_directories(
        Editor_Assets PUBLIC
        "${stb_SOURCE_DIR}"
        "${fast_obj_SOURCE_DIR}"
        "${cgltf_SOURCE_DIR}"
        "${meshoptimizer_SOURCE_DIR}/demo"
        "${lz4_SOURCE_DIR}/lib"
        "${bc7enc_SOURCE_DIR}"
        ..
)
add_library(Editor_Context)
target_sources(Editor_Context PRIVATE
        Context.cpp
        GPUScene.cpp
)
target_link_libraries(
        Editor_Context PUBLIC
        Editor_Assets
        Foundation_RHIVulkan
        Foundation_RenderCore
        Foundation_RenderUtils
)
# https://cmake.org/cmake/help/latest/command/target_precompile_headers.html#:~:text=generator%20expressions
target_precompile_headers(
        Editor_Context
        PUBLIC
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc>"
)

add_executable(Editor)
target_sources(Editor PRIVATE SDLMain.cpp ImGui.cpp Editor.cpp Renderer.cpp PathTracer.cpp)
target_link_libraries(
        Editor PRIVATE Editor_Context Foundation_RHIVulkan ThirdParty_ImGui argh
)
if(WIN32)
    target_link_libraries(Editor PRIVATE comdlg32)
endif()

add_dependencies(
        Editor
        Editor_Shaders
)

add_executable(Editor_Util)
target_sources(Editor_Util PRIVATE Util.cpp)
target_link_libraries(
        Editor_Util PRIVATE
        Editor_Assets
)
