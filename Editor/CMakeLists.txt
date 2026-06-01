cmake_minimum_required(VERSION 3.26)

FetchContent_MakeAvailable(stb)
FetchContent_MakeAvailable(fast_obj)
FetchContent_MakeAvailable(nativefiledialog-extended)
FetchContent_MakeAvailable(meshoptimizer)
FetchContent_MakeAvailable(cgltf)
FetchContent_MakeAvailable(lz4)
FetchContent_MakeAvailable(bc7enc)
FetchContent_MakeAvailable(argh)

add_custom_target(
        Editor_Data
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ASSET_BUILD_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${ASSET_BUILD_DIR}/ViewLUTs"
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                "${CMAKE_SOURCE_DIR}/Source/Renderer/Data"
                "${ASSET_BUILD_DIR}"
        COMMENT "Copying editor data to ${ASSET_BUILD_DIR}"
        VERBATIM
)

add_library(Editor_Assets STATIC)
target_sources(Editor_Assets PUBLIC
        Scene/Mesh.cpp Scene/Scene.cpp
)
target_link_libraries(
        Editor_Assets PUBLIC
        Foundation_GPUScene
        Foundation_Math
        Foundation_Core
        meshoptimizer
)
target_include_directories(
        Editor_Assets PUBLIC
        "${fast_obj_SOURCE_DIR}"
        "${cgltf_SOURCE_DIR}"
        "${meshoptimizer_SOURCE_DIR}/demo"
        ..
)
add_library(Editor_Context)
target_sources(Editor_Context PRIVATE
        Context.cpp
)
target_link_libraries(
        Editor_Context PUBLIC
        Editor_Assets
        Foundation_GPUScene
        Foundation_RHIVulkan
        Foundation_RenderCore
        Foundation_RenderUtils
)

set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc"
        PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(Editor_Context PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc")

# https://cmake.org/cmake/help/latest/command/target_precompile_headers.html#:~:text=generator%20expressions
if (FOUNDATION_USE_PCH)
    target_precompile_headers(
            Editor_Context
            PUBLIC
            "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Precompiled.inc>"
    )
endif()
add_executable(Editor)
target_sources(Editor PRIVATE
        SDLMain.cpp ImGui.cpp
        EditorState.cpp EditorScene.cpp EditorUI.cpp
)
target_link_libraries(
        Editor PRIVATE
        Editor_Context
        Foundation_Pathtracer
        Foundation_Rasterizer
        Foundation_RHIVulkan
        ThirdParty_ImGui argh nfd
)


add_dependencies(
        Editor
        Foundation_Shaders
        Editor_Data
)

add_executable(Editor_Util)
target_sources(Editor_Util PRIVATE Util.cpp)
target_link_libraries(
        Editor_Util PRIVATE
        Editor_Assets
)

add_custom_target(EditorWithUtil DEPENDS Editor Editor_Util)