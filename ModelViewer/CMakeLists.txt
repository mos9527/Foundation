cmake_minimum_required(VERSION 3.26)

file(
    GLOB SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" 
)
file(
    GLOB INCLUDES
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

add_subdirectory(Assets)

target_sources(ModelViewer PRIVATE ${SOURCES} ${INCLUDES} )
target_link_libraries(
    ModelViewer PRIVATE 
    Foundation_Rendering Foundation_RHIVulkan 
    ModelViewer_Assets
    ThirdParty_ImGui
)

add_subdirectory(Shaders)
add_subdirectory(Samples)
add_dependencies(ModelViewer ModelViewer_Shaders ModelViewer_SampleModels)

