cmake_minimum_required(VERSION 3.26)

find_package(Git REQUIRED)
message(STATUS "** Git found: ${GIT_EXECUTABLE} (version ${GIT_VERSION_STRING})")

include(FetchContent)

FetchContent_Declare(
    VulkanMemoryAllocator
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    SPIRV-Headers
    GIT_REPOSITORY https://github.com/KhronosGroup/SPIRV-Headers
    GIT_TAG        vulkan-sdk-1.4.321.0
    GIT_SHALLOW TRUE
)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL
    GIT_TAG release-3.2.26
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.12.2
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG v3.1.5
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.2.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    tinyexr
    GIT_REPOSITORY https://github.com/syoyo/tinyexr.git
    GIT_TAG        v1.0.9
    GIT_SHALLOW TRUE
)
set(TINYEXR_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
set(TINYEXR_USE_MINIZ    ON  CACHE BOOL "" FORCE)

FetchContent_Declare(
    fast_obj
    GIT_REPOSITORY https://github.com/thisistherk/fast_obj
    GIT_TAG v1.3
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf
    GIT_TAG v1.15
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG 19bb5e6fa8da4ba9c333fc6a265d776363c06b39
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG v1.92.7-docking
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    imguizmo
    GIT_REPOSITORY https://github.com/hrydgard/ImGuizmo
    GIT_TAG 5088891e308f14556c6c961aac1be79e1fc1850f
)

FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4
    GIT_TAG v1.10.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    bc7enc
    GIT_REPOSITORY https://github.com/richgel999/bc7enc
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    nativefiledialog-extended
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG v1.2.1
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    argh
    GIT_REPOSITORY https://github.com/adishavit/argh.git
    GIT_TAG v1.3.2
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(stb)
add_library(ThirdParty_STB STATIC stb_image.cpp)
target_include_directories(ThirdParty_STB PUBLIC ${stb_SOURCE_DIR})
# STB_IMAGE on GCC Debug builds without optimization fails to propagate constants to intrinsics -> "argument must be an immediate"
# Force optimization on this specific file/target to fix it.
target_compile_options(ThirdParty_STB PRIVATE $<$<CXX_COMPILER_ID:GNU>:-O2>)

FetchContent_MakeAvailable(tinyexr)
add_library(ThirdParty_TinyEXR STATIC
    tinyexr.cpp
    ${tinyexr_SOURCE_DIR}/deps/miniz/miniz.c
)
target_include_directories(ThirdParty_TinyEXR PUBLIC
    ${tinyexr_SOURCE_DIR}
    ${tinyexr_SOURCE_DIR}/deps/miniz
)
