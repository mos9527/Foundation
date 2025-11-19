cmake_minimum_required(VERSION 3.26)

file(
    GLOB SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

add_executable(Editor)
target_sources(Editor PRIVATE ${SOURCES} ${INCLUDES} )
target_link_libraries(
        Editor PRIVATE
        Foundation_RHIVulkan
        Foundation_RenderCore
        Foundation_RenderUtils
        ThirdParty_ImGui
)

add_subdirectory(Shaders)
target_precompile_headers(
        Editor
        PRIVATE
        Precompiled.inc
)