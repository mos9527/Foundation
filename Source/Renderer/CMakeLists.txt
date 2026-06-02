cmake_minimum_required(VERSION 3.26)

# Used by GPUScene serialize/deserialization
FetchContent_MakeAvailable(stb)
FetchContent_MakeAvailable(lz4)
FetchContent_MakeAvailable(bc7enc)

add_subdirectory(Shaders)
add_library(Foundation_GPUScene STATIC
    GPUScene.cpp
    Precompute.cpp
    Serialization.cpp
    Texture.cpp
    "${lz4_SOURCE_DIR}/lib/lz4.c"
    "${lz4_SOURCE_DIR}/lib/xxhash.c"
    "${bc7enc_SOURCE_DIR}/bc7enc.c"
)
target_link_libraries(Foundation_GPUScene PUBLIC
    Foundation_Core
    Foundation_Math
    Foundation_RenderCore
    Foundation_RHICore
    ThirdParty_STB
)
target_include_directories(Foundation_GPUScene PUBLIC
    ..
    "${stb_SOURCE_DIR}"
    "${lz4_SOURCE_DIR}/lib"
    "${bc7enc_SOURCE_DIR}"
)
add_library(Foundation_Rasterizer STATIC
    Rasterizer.cpp
)
target_link_libraries(Foundation_Rasterizer PUBLIC
    Foundation_GPUScene
    Foundation_RenderUtils
)
target_include_directories(Foundation_Rasterizer PUBLIC ..)

add_library(Foundation_Pathtracer STATIC
        Pathtracer.cpp
)
target_link_libraries(Foundation_Pathtracer PUBLIC
    Foundation_GPUScene
    Foundation_RenderUtils
)
target_include_directories(Foundation_Pathtracer PUBLIC ..)
