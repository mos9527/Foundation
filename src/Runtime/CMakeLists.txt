cmake_minimum_required(VERSION 3.26)

add_library(Foundation_Runtime INTERFACE)

add_subdirectory(Core)
add_subdirectory(Bits)
add_subdirectory(Blobs)
add_subdirectory(Math)
add_subdirectory(RHICore)
add_subdirectory(RHIVulkan)

target_link_libraries(Foundation_Runtime
    INTERFACE
    Foundation_Core
    Foundation_Platform
    Foundation_Bits
    Foundation_Math
    Foundation_RHICore
    Foundation_RHIVulkan
    Foundation_Blobs
)
target_include_directories(Foundation_Runtime
    INTERFACE
    ..
)
