cmake_minimum_required(VERSION 3.26)

add_library(Foundation_Core INTERFACE)

add_subdirectory(Allocator)
add_subdirectory(Container)
add_subdirectory(Logging)

target_link_libraries(Foundation_Core
    INTERFACE
    Foundation_Allocator
    Foundation_Container
    Foundation_Logging
)

