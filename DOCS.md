Foundation {#mainpage}
===
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/eb139a29588a4f6d94da55e93e0239c8)](https://app.codacy.com/gh/mos9527/Foundation/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

[Foundation](https://github.com/mos9527/Foundation/) is a work-in-progress cross-platform rendering framework.

Heavily inspired by Arseny Kapoulkine's [niagara](https://github.com/zeux/niagara),
[bgfx](https://github.com/bkaradzic/bgfx), and [Unreal Engine](https://www.unrealengine.com/en-US/),
this project aims to provide a high-performance, low overhead rendering framework for *extremely*
fast prototyping of various GPU workloads.

Features
---
- Low-level modern API (Vulkan 1.3, DirectX12) as first-class citizen
- Modern C++20 codebase with minimal dependencies 
- Modern lock-free data structures at @ref Foundation::Atomics for low-contention, high-concurrency workloads
- Explict thread safe guarantees - you pay for what you use
- Arena allocation strategies for minimal fragmentation and latency in hot paths
- Optional profiling integration with [Tracy Profiler](https://github.com/wolfpld/tracy)

Renderer
---
- Full SPIR-V shader reflection support with automatic pipeline binding and generation
- Frame Graph/Frame Pass architecture with optimized resource barrier placement
- Async Compute support for modern GPUs with automatic release/acquire and synchronization
- Multithreaded command recording with automatic command buffer merging
- [Unreal Render Dependency Graph](https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine) inspired
  syntax without esoteric macros


Quickstart
---
A comprehensive Examples section is provided below for quickstarts and reference.

You can also check out the @ref ModelViewer application for an advanced usage of the framework.

Examples
---
All examples can also be found at <a href="examples.html">The Examples</a> directory.

Building
---
`cmake` is required for builds. A C++20 compliant compiler is also required.

All third-party dependencies are included as [`FetchContent`](https://cmake.org/cmake/help/latest/module/FetchContent.html) declarations. See @ref Thirdparty for a comprehensive list.

[Slang](https://shader-slang.com) is required for building shaders for all backends, and should be available in your `PATH`.

[Vulkan SDK](https://vulkan.lunarg.com/) is required for building the @ref VulkanApplication backend.

### Windows
You can build, and debug the app with [Visual Studio's CMake intergration](https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio?view=msvc-170#ide-integration). Or with any alternative CMake workflow of your choice.

The Vulkan SDK installer should take care of most, if not all the setup for you.

### Linux
Refer to https://docs.vulkan.org/tutorial/latest/02_Development_environment.html#_linux_2 for setting up the Vulkan SDK on Linux.

### macOS
Refer to https://docs.vulkan.org/tutorial/latest/02_Development_environment.html#_macos for setting up the Vulkan SDK on macOS.

### Building from command line
The following commands will create a build directory, generate the build system files, and build all targets with 8 parallel jobs.
Binary artifacts will be located in `build/bin/`.

```bash
mkdir build
cd build
cmake ..
cmake --build . -j8
```

### Build Options
The following CMake options are available:
| Option | Description | Default |
|--------|-------------|---------|
| FOUNDATION_WITH_SANITIZERS | Build with sanitizers enabled | OFF |
| FOUNDATION_WITH_PROFILING | Build with profiler (Tracy) enabled | ON |
| FOUNDATION_RHIVULKAN_VALIDATION_LAYER | Build with Vulkan Validation Layer enabled | ON |
| FOUNDATION_WITH_EXAMPLES | Build examples | ON |
| FOUNDATION_WITH_TESTS | Build tests | ON |
| FOUNDATION_WITH_MODELVIEWER | Build the model viewer application | ON |

Toggle these options with `-D<OPTION>=ON/OFF` when running `cmake ..`, e.g. `cmake -DFOUNDATION_WITH_SANITIZERS=ON ..`

[CMake Unity Builds](https://cmake.org/cmake/help/latest/prop_tgt/UNITY_BUILD.html) are supported, and can be enabled with `-DCMAKE_UNITY_BUILD=ON` when running `cmake ..`.


Third party
---
### Core
- https://github.com/microsoft/mimalloc.git
- https://github.com/fmtlib/fmt.git
- https://github.com/gabime/spdlog.git
- https://github.com/wolfpld/tracy.git
### Math
- https://github.com/g-truc/glm.git
### Native
- https://github.com/glfw/glfw.git
- https://tinyfiledialogs.sourceforge.net/
### RHIVulkan
- https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
### ModelViewer
- https://github.com/nothings/stb.git
- https://github.com/thisistherk/fast_obj
- https://github.com/zeux/meshoptimizer.git

Debugging
---
Or, notes to self. If you are *here*, well ...thanks a lot for the interest^^
### Vulkan
- [VVL Timeline resource tracking](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/2441) has been an issue - though should be resolved
by [now](https://github.com/KhronosGroup/Vulkan-ValidationLayers/pull/10316). Always update the SDK to the latest version.
- [RenderDoc](https://renderdoc.org/) is generally enough for most debugging tasks.
- Though some esoteric issues would eventually require HW specific tools for accurate timing and performance metrics.
  - [NSight™ Graphics](https://developer.nvidia.com/nsight-graphics) for NVIDIA GPUs
  - [Radeon™ Developer Tool Suite](https://gpuopen.com/news/introducing-radeon-developer-tool-suite/)

### Linux & AMD
[Radeon™ Developer Tool Suite](https://gpuopen.com/news/introducing-radeon-developer-tool-suite/) has issues
with OSS AMD drivers (RADV and co), see:
- https://github.com/GPUOpen-Tools/radeon_developer_panel/issues/36
- https://github.com/GPUOpen-Tools/radeon_developer_panel/issues/57

On Arch `https://aur.archlinux.org/packages/vulkan-amdgpu-pro` seems to resolve the issue.