cmake_minimum_required(VERSION 3.26)
project(Foundation)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 20)


list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/Source")

add_subdirectory(Core)
add_subdirectory(Math)
add_subdirectory(RenderCore)
add_subdirectory(RenderUtils)
add_subdirectory(RHICore)
add_subdirectory(RHIVulkan)

add_library(Foundation INTERFACE)
target_link_libraries(Foundation INTERFACE
    Foundation_Core
    Foundation_Math
    Foundation_RenderCore
    Foundation_RenderUtils
)
