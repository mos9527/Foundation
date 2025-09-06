Foundation
===

Building
---
`cmake` is required for builds. A C++20 compliant compiler is also required.

All third-party dependencies are included as [`FetchContent`](https://cmake.org/cmake/help/latest/module/FetchContent.html) declarations. See @ref Thirdparty for a comprehensive list.

[Slang](https://shader-slang.com) is required for building shaders for all backends, and should be available in your `PATH`.

[Vulkan SDK](https://vulkan.lunarg.com/) is required for building the @ref VulkanApplication backend.

### Windows
You can build, and debug the app with [Visual Studio's CMake intergration](https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio?view=msvc-170#ide-integration). Or with any alternative CMake workflow of your choice.

The Vulkan SDK installer should take care of most, if not all of the setup for you.

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

Quick Start
---
### Backends
#### @ref VulkanApplication
A Vulkan 1.3 compatible GPU and drivers are required to run the @ref VulkanApplication backend.

This is the only @ref RHIApplication implementation currently available.
### Usage
The following classes can be used to create a basic rendering application:
- @ref Foundation::Rendering::RenderApplication
- @ref Foundation::Rendering::Renderer

See @ref Examples for reference implementations and usage.

Model Viewer
---
@ref ModelViewer demonstrates a simple model viewer application built using @ref Foundation.

See @ref ModelViewer page for source and more details.

Namespaces
---
<a href="namespaces.html">List of all namespaces</a>

Thirdparty
---
### @ref Core
- https://github.com/microsoft/mimalloc.git
### @ref Runtime
- https://github.com/fmtlib/fmt.git
- https://github.com/gabime/spdlog.git
### @ref Math
- https://github.com/g-truc/glm.git
### @ref NativeApplication
- https://github.com/glfw/glfw.git
- https://tinyfiledialogs.sourceforge.net/
### @ref RHIVulkan
- https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
### @ref ModelViewer
- https://github.com/nothings/stb.git
- https://github.com/thisistherk/fast_obj
- https://github.com/zeux/meshoptimizer.git
