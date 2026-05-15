Foundation {#mainpage}
===
[Foundation](https://github.com/mos9527/Foundation/) is a work-in-progress cross-platform rendering framework.

Editor
---
![fatguy](https://github.com/user-attachments/assets/233fc6d8-f3c4-4e68-b4c3-b458f142475f)

Editor houses our implementation of the Reference Unidirectional Path Tracer `PT` and Rasterizer `RASTER`.

### Foundation Material Model
#### Principled Material
Layered PBR material interface based on the [OpenPBR model](https://academysoftwarefoundation.github.io/OpenPBR) (as seen in Blender's [Principled BSDF](https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/principled.html)), offering full support with the exception of [3.8 Thin-film iridescence](https://academysoftwarefoundation.github.io/OpenPBR/#model/thin-filmiridescence)
![Blender BSDF](https://docs.blender.org/manual/en/latest/_images/render_shader-nodes_principled_layers.svg)
#### Principled Hair Material
[Principled Hair BSDF](https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/hair_principled.html)'s Chiang single-scattering model implementation. Many bounces may be required to derive accurate hair scattering phenomena.

### Color Pipeline
- OCIO-based SDR & HDR color pipeline
- Linear BT.709 scene space (D65), converted to D60 via Bradford CAT and encoded as AP1
- ACEScct log-space encoding for grading and display transforms
- LUT-based output transforms for SDR/HDR — ACES 1.3, ACES 2.0, AgX, Standard (sRGB and PQ)
- 1-to-1 match with Blender OCIO output

### Path Tracer
- **Full Foundation Material support**
- Unidirectional integrator with tiled sampling
- Sobol & PCG samplers
- Path-traced Subsurface scattering
- Path-traced Chiang Hair/fur shading
- Curve rendering for hair/fur, etc
- Importance sampled environment maps
- Anamorphic physical lens
- Area, point, spot, and directional lights
- Uniform/Power light sampling with alias tables
- Runs entirely in your Vulkan GPU :)

### Rasterizer
- GPU-driven mesh shader pipeline with hierarchical continuous LOD
- Two-phase meshlet occlusion culling
- Optional RT Shadows

### Scene
- Extended glTF support & Blender data exchange via https://github.com/mos9527/Foundation-Blender-IO/tree/main
- Custom binary scene format `FSCN` with excellent serialization/loading (memory-mapped) performance

Examples
---
Smaller examples, using the framework as a library, are provided for reference and testing purposes.

These be found in <a href="examples.html">The Examples</a> directory.

TODO SCREENSHOTS

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

- Arch Linux
```bash
# Enable [extra-testing] in /etc/pacman.conf for the latest validation layers et al
sudo pacman -S vulkan-validation-layers vulkan-tools vulkan-radeon vulkan-headers
```

### macOS
Install the official [Vulkan SDK](https://vulkan.lunarg.com/).

The Editor will build but not run on this platform, due to our extensive usage of Shader Binding Tables and Mesh Shaders,
which are not sup

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
| FOUNDATION_WITH_ASAN | Build with Address Sanitizer enabled | OFF |
| FOUNDATION_WITH_TSAN | Build with Thread Sanitizer enabled | OFF |
| FOUNDATION_WITH_PROFILING | Build with profiler (Tracy) enabled | ON |
| FOUNDATION_RHIVULKAN_VALIDATION_LAYER | Build with Vulkan Validation Layer enabled | ON |
| FOUNDATION_WITH_EXAMPLES | Build examples | ON |
| FOUNDATION_WITH_TESTS | Build tests | ON |

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
### RHIVulkan
- https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
### Editor, Examples & Tests
- https://github.com/nothings/stb.git
- https://github.com/thisistherk/fast_obj
- https://github.com/zeux/meshoptimizer.git

Debugging
---
### Vulkan
- [VVL Timeline resource tracking](https://github.com/KhronosGroup/Vulkan-ValidationLayers/issues/2441) has been an issue - though should be resolved
by [now](https://github.com/KhronosGroup/Vulkan-ValidationLayers/pull/10316). Always update the SDK to the latest version.
- [RenderDoc](https://renderdoc.org/) is generally enough for most debugging tasks.
- Though some esoteric issues would eventually require HW specific tools for accurate timing and performance metrics.
  - [NSight™ Graphics](https://developer.nvidia.com/nsight-graphics) for NVIDIA GPUs
  - [Radeon™ Developer Tool Suite](https://gpuopen.com/news/introducing-radeon-developer-tool-suite/)

clang-tidy
===
```bash
mkdir build
cd build
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
# Get the script from https://github.com/llvm-mirror/clang-tools-extra/blob/master/clang-tidy/tool/run-clang-tidy.py
python run-clang-tidy.py -header-filter=".*/Source/.*" ".*/Source/.*" -config "$(cat ../.clang-tidy)"
```