cmake_minimum_required(VERSION 3.26)

include(FetchContent)
FetchContent_MakeAvailable(stb)
FetchContent_MakeAvailable(fast_obj)
FetchContent_MakeAvailable(cgltf)
FetchContent_MakeAvailable(meshoptimizer)

file(
    GLOB_RECURSE SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
file(
    GLOB_RECURSE INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

add_library(ModelViewer_Assets)
target_sources(ModelViewer_Assets PRIVATE ${SOURCES} ${INCLUDES})
target_link_libraries(ModelViewer_Assets PUBLIC Foundation_Core Foundation_Math meshoptimizer)
target_include_directories(ModelViewer_Assets PUBLIC "${stb_SOURCE_DIR}" "${fast_obj_SOURCE_DIR}" "${cgltf_SOURCE_DIR}")
target_include_directories(
    ModelViewer_Assets
    PUBLIC
    ..
)
