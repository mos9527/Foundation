cmake_minimum_required(VERSION 3.26)

file(
    GLOB_RECURSE SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB_RECURSE INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

add_library(ModelViewer_Scene)
target_sources(ModelViewer_Scene PRIVATE ${SOURCES} ${INCLUDES})
target_link_libraries(ModelViewer_Scene PUBLIC Foundation_Core Foundation_Native Foundation_Rendering ModelViewer_Assets)
target_include_directories(
    ModelViewer_Scene
    PUBLIC
    ..
)
