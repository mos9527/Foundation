cmake_minimum_required(VERSION 3.26)

add_library(Foundation_Core INTERFACE)

add_subdirectory(Allocator)
add_subdirectory(Container)
add_subdirectory(Platform)

include(FetchContent)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        11.2.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.3
  GIT_SHALLOW TRUE
)
set(SPDLOG_FMT_EXTERNAL ON)
FetchContent_MakeAvailable(spdlog)

target_link_libraries(Foundation_Core
    INTERFACE
    Foundation_Allocator
    Foundation_Container
    Foundation_Platform
)
