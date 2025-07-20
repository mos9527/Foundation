cmake_minimum_required(VERSION 3.26)

add_library(Foundation_Core INTERFACE)

include(FetchContent)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        11.2.0
)
FetchContent_MakeAvailable(fmt)

add_subdirectory(Allocator)
add_subdirectory(Platform)

target_link_libraries(Foundation_Core
    INTERFACE
    Foundation_Allocator
    Foundation_Platform
)
