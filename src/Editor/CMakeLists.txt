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
    add_executable(Foundation_Editor WIN32)
elseif (APPLE)
    add_executable(Foundation_Editor)
elseif (UNIX)
    add_executable(Foundation_Editor)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

target_sources(Foundation_Editor PRIVATE ${SOURCES} ${INCLUDES} "Editor.cpp")
target_link_libraries(Foundation_Editor PRIVATE Foundation_Core)
target_link_libraries(Foundation_Editor PRIVATE Foundation_Platform)
target_link_libraries(Foundation_Editor PRIVATE Foundation_Renderer)
