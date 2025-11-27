cmake_minimum_required(VERSION 3.26)

FetchContent_MakeAvailable(stb)
FetchContent_MakeAvailable(fast_obj)
FetchContent_MakeAvailable(meshoptimizer)
FetchContent_MakeAvailable(cgltf)

file(
    GLOB SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

add_subdirectory(Shaders)
add_library(Editor_Assets)
target_sources(Editor_Assets PUBLIC Mesh.cpp Mesh.hpp)
target_link_libraries(
    Editor_Assets PUBLIC
    Foundation_Math
    Foundation_Core
    meshoptimizer
)
target_include_directories(
    Editor_Assets PUBLIC "${stb_SOURCE_DIR}" "${fast_obj_SOURCE_DIR}" "${cgltf_SOURCE_DIR}" "${meshoptimizer_SOURCE_DIR}/demo" ..
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

target_precompile_headers(
        Editor
        PRIVATE
        Precompiled.inc
)
add_dependencies(
        Editor
        Shader_ECSIndirectTaskGen
        Shader_ECSInstanceTaskCull
        Shader_EMSBasic
        Shader_ETSMeshletCull
        Shader_EPSBasic
        Shader_EPSBlit
        Shader_ECSOverdrawClear
        Shader_ECSOverdrawReduce
        Examples_SampleFiles
)