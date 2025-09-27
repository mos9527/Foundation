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


target_sources(ModelViewer PRIVATE ${SOURCES} ${INCLUDES} "App.cpp")
target_link_libraries(ModelViewer PRIVATE Foundation_Rendering Foundation_RHIVulkan meshoptimizer)
target_include_directories(ModelViewer PRIVATE ../src "${stb_SOURCE_DIR}" "${fast_obj_SOURCE_DIR}" "${cgltf_SOURCE_DIR}")

add_subdirectory(Shaders)
add_subdirectory(Samples)
add_dependencies(ModelViewer ModelViewer_Shaders ModelViewer_SampleModels)

