Foundation
===
Foundation is a work-in-progress cross-platform rendering framework.

Heavily inspired by Arseny Kapoulkine's [niagara](https://github.com/zeux/niagara),
[bgfx](https://github.com/bkaradzic/bgfx), and [Unreal Engine](https://www.unrealengine.com/en-US/),
this project aims to provide a high-performance, low overhead rendering framework for *extremely*
fast prototyping of various GPU workloads.

Features
---
- Low-level modern API (Vulkan 1.3, DirectX12) as first-class citizen
- Modern C++20 codebase with minimal dependencies
- Full SPIR-V shader reflection support with automatic pipeline binding and generation
- Frame Graph/Frame Pass architecture with optimized resource barrier placement
- Async Compute support for modern GPUs with automatic synchronization
- Headless rendering support for GPGPU tasks @ref Foundation::Rendering::RenderApplication
- Convenient in-built rendering techniques @ref Foundation::Rendering::createPSFullscreenPass, @ref Foundation::Rendering::createPSBackbufferBlitPass, etc.
- Cross-platform support for Windows, Linux, and macOS
  - Vulkan-like explicit RHI APIs @ref Foundation::RHI
  - DirectX-like rendering concepts at @ref Foundation::Rendering
  - WinAPI-like application APIs @ref Foundation::Native::NativeApplication

Quickstart
---
<a href="examples.html">The Examples</a> is a great place to start exploring the framework.

Feel free to explore the documentation, or check out the [Model Viewer](#model-viewer) application for a more complete example.

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
The following commands will create a build directory, generate the build system files, and build the project
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
| FOUNDATION_WITH_EXAMPLES | Build examples | ON |
| FOUNDATION_WITH_TESTS | Build tests | ON |
| FOUNDATION_WITH_MODELVIEWER | Build the model viewer application | ON |

Toggle these options with `-D<OPTION>=ON/OFF` when running `cmake ..`, e.g. `cmake -DFOUNDATION_WITH_SANITIZERS=ON ..`

Model Viewer
---
@ref ModelViewer demonstrates a simple model viewer application built using @ref Foundation.

See @ref ModelViewer page for source and more details.

Third party
---
### Core
- https://github.com/microsoft/mimalloc.git
### Runtime
- https://github.com/fmtlib/fmt.git
- https://github.com/gabime/spdlog.git
### Math
- https://github.com/g-truc/glm.git
### NativeApplication
- https://github.com/glfw/glfw.git
- https://tinyfiledialogs.sourceforge.net/
### RHIVulkan
- https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
### ModelViewer
- https://github.com/nothings/stb.git
- https://github.com/thisistherk/fast_obj
- https://github.com/zeux/meshoptimizer.git
