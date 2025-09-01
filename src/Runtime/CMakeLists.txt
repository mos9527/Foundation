cmake_minimum_required(VERSION 3.26)

add_library(Foundation_Runtime INTERFACE)

# -- Third party dependencies
include(FetchContent)

FetchContent_Declare(
  mimalloc
  GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
  GIT_TAG        v3.1.5
)
FetchContent_MakeAvailable(mimalloc)

FetchContent_Declare(
  VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG        v3.3.0
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

FetchContent_Declare(
  GLFW
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG        506c11ba43b901dbcc4d90449f46de67cf000af4
)
FetchContent_MakeAvailable(GLFW)

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        11.2.0
)
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.15.3
)
set(SPDLOG_FMT_EXTERNAL ON)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
  glm
  GIT_REPOSITORY    
  GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

FetchContent_Declare(
  meshoptimizer
  GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
  GIT_TAG        e8879f358d9dccf3038dd194e99d2749347da1dd
)
FetchContent_MakeAvailable(meshoptimizer)
# -- End third party dependencies

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
