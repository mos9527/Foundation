cmake_minimum_required(VERSION 3.26)

include(FetchContent)

FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(stb)

FetchContent_Declare(
        fast_obj
        GIT_REPOSITORY https://github.com/thisistherk/fast_obj
        GIT_TAG v1.3
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fast_obj)

FetchContent_Declare(
        cgltf
        GIT_REPOSITORY https://github.com/jkuhlmann/cgltf
        GIT_TAG v1.15
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cgltf)

FetchContent_Declare(
        meshoptimizer
        GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
        GIT_TAG v0.25
        GIT_SHALLOW TRUE
)
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
target_link_libraries(ModelViewer_Assets PUBLIC Foundation_Core Foundation_Math Foundation_Bits Foundation_Native Foundation_Rendering meshoptimizer)
target_include_directories(ModelViewer_Assets PUBLIC "${stb_SOURCE_DIR}" "${fast_obj_SOURCE_DIR}" "${cgltf_SOURCE_DIR}")
target_include_directories(
    ModelViewer_Assets
    PUBLIC
    ..
)
