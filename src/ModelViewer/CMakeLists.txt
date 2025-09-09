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
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})

if (WIN32)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /ENTRY:mainCRTStartup")
    add_executable(ModelViewer WIN32)
elseif (APPLE)
    add_executable(ModelViewer)
elseif (UNIX)
    add_executable(ModelViewer)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()


target_sources(ModelViewer PRIVATE ${SOURCES} ${INCLUDES} "ModelViewer.cpp")
target_link_libraries(ModelViewer PRIVATE Foundation_Runtime Foundation_Rendering meshoptimizer)
target_include_directories(ModelViewer PRIVATE .. "${stb_SOURCE_DIR}" "${fast_obj_SOURCE_DIR}")

add_subdirectory(Shaders)
add_dependencies(ModelViewer
    Shader_MVClearCounters
    Shader_MVIndirectCull
    Shader_Mesh
)

