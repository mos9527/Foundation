cmake_minimum_required(VERSION 3.26)

add_library(Foundation_Core INTERFACE)

add_subdirectory(Allocator)
add_subdirectory(Container)


target_link_libraries(Foundation_Core
    INTERFACE
    Foundation_Allocator
    Foundation_Container
)
