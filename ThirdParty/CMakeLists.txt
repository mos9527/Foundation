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

# Foundation only uses SDL for windowing + Vulkan surface. Disable every
# subsystem we don't touch so SDL's per-subsystem platform probes (DirectX,
# WASAPI, HIDAPI, OpenGL/EGL/GLX, Media Foundation, ...) are skipped at
# configure time. Keep VIDEO (mouse/keyboard arrive via video events);
# SDL_VULKAN is auto-enabled because VIDEO is on.
if (ANDROID)
    # Android's SDLActivity.java explicitly calls System.loadLibrary("SDL3");
    # We must build it as a shared library so libSDL3.so is packaged into the APK.
    set(SDL_SHARED               ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC               OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED               OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC               ON  CACHE BOOL "" FORCE)
endif()
set(SDL_TEST_LIBRARY         OFF CACHE BOOL "" FORCE)
set(SDL_TESTS                OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL              OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL_TESTS        OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL      ON  CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL_DOCS ON  CACHE BOOL "" FORCE)

set(SDL_AUDIO   OFF CACHE BOOL "" FORCE)
set(SDL_RENDER  OFF CACHE BOOL "" FORCE)   # we render via Vulkan ourselves
set(SDL_GPU     OFF CACHE BOOL "" FORCE)   # ditto
set(SDL_CAMERA  OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC  OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI  ON CACHE BOOL "" FORCE) # Must be ON for Android; SDL3 unconditionally references HIDDeviceManager_tab
set(SDL_POWER   OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR  OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG  ON CACHE BOOL "" FORCE)   # native file open/save dialogs
set(SDL_OPENGL  OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_DIRECTX OFF CACHE BOOL "" FORCE)   # kills D3D/WASAPI SDK probes on Windows
set(SDL_DUMMYAUDIO OFF CACHE BOOL "" FORCE)
set(SDL_OFFSCREEN  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.26
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SDL3)

if (ANDROID)
    # Copy SDL3 Java sources to the Android project so Gradle can find them
    file(COPY "${sdl3_SOURCE_DIR}/android-project/app/src/main/java/org"
         DESTINATION "${CMAKE_SOURCE_DIR}/Android/app/src/main/java")
endif()

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG v0.13.1
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
    fast_obj
    GIT_REPOSITORY https://github.com/thisistherk/fast_obj
    GIT_TAG v1.3
    GIT_SHALLOW TRUE
)

add_library(cgltf INTERFACE)
target_include_directories(cgltf INTERFACE "${CMAKE_CURRENT_LIST_DIR}")

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
