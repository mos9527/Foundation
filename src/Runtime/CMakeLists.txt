cmake_minimum_required(VERSION 3.26)

add_library(Foundation_Runtime INTERFACE)

add_subdirectory(Core)
add_subdirectory(Bits)
add_subdirectory(Math)
add_subdirectory(Native)
add_subdirectory(RHICore)
add_subdirectory(RHIVulkan)

target_link_libraries(Foundation_Runtime
    INTERFACE
    Foundation_Core
    Foundation_Bits
    Foundation_Math
    Foundation_Native
    Foundation_RHICore
    Foundation_RHIVulkan
)
target_include_directories(Foundation_Runtime
    INTERFACE
    ..
)
