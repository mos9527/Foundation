cmake_minimum_required(VERSION 3.26)

file(
    GLOB_RECURSE SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB_RECURSE INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

add_library(ModelViewer_Gizmos)
target_sources(ModelViewer_Gizmos PRIVATE ${SOURCES} ${INCLUDES})
target_link_libraries(ModelViewer_Gizmos PUBLIC ThirdParty_ImGui Foundation_Core Foundation_Math)
