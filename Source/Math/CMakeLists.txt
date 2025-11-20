cmake_minimum_required(VERSION 3.26)

file(
    GLOB_RECURSE SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp" 
)
file(
    GLOB_RECURSE INCLUDES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
)

include(FetchContent)
FetchContent_MakeAvailable(glm)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    set(GLM_ENABLE_SIMD_AVX2 ON CACHE BOOL "" FORCE)
endif()
set(GLM_ENABLE_CXX_20 ON CACHE BOOL "" FORCE)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCES} ${INCLUDES})
add_library(Foundation_Math ${SOURCES} ${INCLUDES})
target_link_libraries(Foundation_Math PUBLIC glm)
set_target_properties(Foundation_Math PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(
    Foundation_Math
    PUBLIC
    ..
)
