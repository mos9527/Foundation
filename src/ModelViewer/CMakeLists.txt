cmake_minimum_required(VERSION 3.26)

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
    add_executable(Foundation_ModelViewer WIN32)
elseif (APPLE)
    add_executable(Foundation_ModelViewer)
elseif (UNIX)
    add_executable(Foundation_ModelViewer)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

target_sources(Foundation_ModelViewer PRIVATE ${SOURCES} ${INCLUDES} "ModelViewer.cpp")
target_link_libraries(Foundation_ModelViewer PRIVATE Foundation_Runtime Foundation_Renderer Foundation_Cooking)
target_include_directories(Foundation_ModelViewer PUBLIC ..)
