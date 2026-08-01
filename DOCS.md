Foundation {#mainpage}
===

[Foundation](https://github.com/mos9527/Foundation/) is a work-in-progress cross-platform rendering framework/renderer.

Foundation includes three bespoke renderers - The Progressive Path Tracer `PPT` *based* on [Physically Based Rendering:From Theory To Implementation](https://www.pbr-book.org/), a Continuous LOD Meshlet (Nanite-like) Rasterizer `RASTER`,
and a Real-time Pathtracer `RTPT`

Please note that the Path Tracer is not a faithful recreation of PBRT - however heavily referenced against, with outputs extensively verified against results from [Blender Cycles](https://projects.blender.org/blender/cycles/) et al. `PT` outputs can be considered reference in this context.

Canonical glTF format scenes are supported, alongside ones with private extensions made for Foundation itself. https://github.com/mos9527/Foundation-Blender-IO is used for scene interchange  between Blender and Foundation.

Sample scene and resources can be found at https://github.com/mos9527/Foundation-Resources

### Material Models
#### Principled Material
Layered PBR material interface based on the [OpenPBR model](https://academysoftwarefoundation.github.io/OpenPBR) (as seen in Blender's [Principled BSDF](https://docs.blender.org/manual/en/latest/render/shader_nodes/shader/principled.html)), offering full support with the exception of [3.8 Thin-film iridescence](https://academysoftwarefoundation.github.io/OpenPBR/#model/thin-filmiridescence)

<img src="https://raw.githubusercontent.com/mos9527/Foundation/refs/heads/dev/Images/blender-principled-bsdf-layers.svg" alt="Blender Principled BSDF" style="width:100%;max-width:100%;display:block;">

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
- Unidirectional megakernel integrator
- Energy conserving BSDFs with Cycles output parity 
- Sobol & PCG samplers
- Path-traced Subsurface scattering
- Path-traced Chiang Hair/fur shading
- Curve rendering for hair/fur, etc
- Importance sampled environment maps
- Anamorphic physical lens
- Area, point, spot, and directional lights
- Context-dependent Light BVH sampling for analytical lights


### Rasterizer
- GPU-driven mesh shader pipeline with hierarchical continuous LOD
- Two-phase meshlet occlusion culling
- Optional RT Shadows

### Realtime Path Tracer
- Basic PBR material support
- [SHARC](https://github.com/NVIDIA-RTX/SHARC) integration

### Scene
- Extended glTF support & Blender data exchange via https://github.com/mos9527/Foundation-Blender-IO/tree/main
- Custom binary scene format `FSCN` with excellent serialization/loading (memory-mapped) performance
- Animation (Skinning, BlendShapes, Articulated rigid bodies)

Examples
---
Smaller examples, using the framework as a library (therefore excluding the renderers) are provided for reference and testing purposes.

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

CI Builds are provided for this platform. You can get them from [nightly](https://nightly.link/mos9527/Foundation/workflows/build/dev)

### Linux
Refer to https://docs.vulkan.org/tutorial/latest/02_Development_environment.html#_linux_2 for setting up the Vulkan SDK on Linux.

- Arch Linux
```bash
# Enable [extra-testing] in /etc/pacman.conf for the latest validation layers et al
sudo pacman -S vulkan-validation-layers vulkan-tools vulkan-radeon vulkan-headers
```

### macOS
Install the official [Vulkan SDK](https://vulkan.lunarg.com/) PKG, and expect things to just build through CMake as usual.

### Android
Open `Android/` as a project in Android Studio ~~and suffer~~ - that should (hopefully) be enough.

Notes on example selection - for CI and Android only select examples are built. These are populated by:
```bash
python Scripts/GenerateExamples.py
```

### Building from command line
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
| FOUNDATION_NO_EXCEPTIONS | Build without exceptions | ON |
| FOUNDATION_RHIVULKAN_VALIDATION_LAYER | Build with Vulkan Validation Layer enabled | ON |
| FOUNDATION_WITH_ASAN | Build with Address Sanitizer enabled | OFF |
| FOUNDATION_WITH_TSAN | Build with Thread Sanitizer enabled | OFF |
| FOUNDATION_WITH_PROFILING | Build with profiler (Tracy) enabled | ON |
| FOUNDATION_WITH_EXAMPLES | Build examples | ON |
| FOUNDATION_WITH_JOLT_EXAMPLES | Build examples that require Jolt Physics | OFF |
| FOUNDATION_OS_ALLOCATOR | Build with OS allocator, instead of using `mimalloc` | ON |

- NOTE: On MinGW GCC, you might need `-DFOUNDATION_OS_ALLOCATOR=ON` to use the OS allocator should you encounter crashes into `ntdll!RtlFreeHeap`.

Toggle these options with `-D<OPTION>=ON/OFF` when running `cmake ..`, e.g. `cmake -FOUNDATION_WITH_ASAN=ON ..`

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


TODOs
===
Framework
---
We use Vulkan exclusively, so portability wise:
- [x] Desktop (Windows & Linux) is probably the only platform we truly care about
  - RT & Mesh Shader usage practically means anything DirectX 12 Ultimate Certified will Just Work(tm)
  - [ ] TODO Fallback path for Raster to invoke MDI when running without Mesh Shader support?
- [-] Android 
  - Inline Ray Tracing is surprisingly solid & widely supported on mobile Androids *nowadays*
  - As ridiculous as it sounds - GPUScene Examples can run on those devices. Turns out you *can* do HW accelerated Path Tracing on mobile...
  - Mesh Shaders are mostly amiss. Newer (8Gen3) Adrenos offer support - not sure about other chips.   
- [-] Metal (iDevices and Macs)  
  - [MoltenVK](https://github.com/KhronosGroup/MoltenVK), [KosmickKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html)
  - Neither supports RT nor Mesh Shaders.
  - Other examples work on both. That, and Mesa `llvmpipe` (headless)... 
- [x] Lavapipe
  - https://www.vulkan.org/user/pages/09.events/vulkanised-2025/T5-Lucas-Fryzek-Igalia.pdf
  - Get your own build.
    - Windows: https://github.com/pal1000/mesa-dist-win/releases
    - macOS: Try https://github.com/rerun-io/lavapipe-build script to build from source
    - Linux: https://docs.mesa3d.org/install.html      
  - Set `VK_DRIVER_FILES` environ.
    - Powershell: ` $env:VK_DRIVER_FILES="C:\Users\komahuang\Downloads\mesa3d-26.1.3-release-msvc\x64\lvp_icd.x86_64.json"` - note the full path, change this to your own.
    - Bash et al: `export VK_DRIVER_FILES="/path/to/lvp_icd.x86_64.json"`
  - Run stuff from the same shell.

Editor
---
- [x] **Correct** SDR&HDR Color Pipeline
  - Linear BT709 Scene Space (D65), converted to D60 via [Bradford CAT](http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html) and encoded as [AP1](https://docs.acescentral.com/encodings/aces2065-1/#transfer-function)
  - Transform then encodes as [ACEScct](https://docs.acescentral.com/encodings/acescct/#encoding-function)
  - Rest of the transform handled by LUTs in ACEScct log space, incl. to display EOTF. See `Scripts/OCIOBakeLUTs.py`
  - Blender OCIO Config used to generated LUTs for SDR/HDR ACES1.3/2.0/AgX/Standard (sRGB. v. PQ) transforms
    - See also https://docs.blender.org/manual/en/latest/render/color_management/
    - Obtains 1-to-1 matching output :)

- [ ] Coroutines in place of...whatever this is.
Good to have, not necessary.
- [ ] Scene Graph, instead of AoS to represent instances.
Done
- [x] Animation (Skinning, BlendShapes, Articulated rigid bodies, camera & lights. Contribution by Claude)

Progressive Path Tracer
---
Sorted by priority (high to low), also w/ future blog post series update on:
- [ ] Emissive triangles in NEE
- [ ] Tracing from surfaces (for baking lightmaps, probe generation, etc)

Done, awaiting Blog Update:
- [x] Adaptive Sampling (https://jo.dreggn.org/home/2009_stopping.pdf)
  - Per-pixel then dialated with a box filter. Same as Cycles.
  - Kills the warps to skip converged pixels, not regenerating paths as discussed in [Megakernels Considered Harmful: Wavefront Path Tracing on GPUs](https://research.nvidia.com/sites/default/files/pubs/2013-07_Megakernels-Considered-Harmful/laine2013hpg_paper.pdf)
    - TODO? Perhaps unnecessary if we'd actually move to wavefronts...
- [x] Hair Shading
  - [A Practical and Controllable Hair and Fur Model for Production Path Tracing [Chiang et al. 2016]](https://sci-hub.sg/storage/2024/5766/a1c3a0a1d0aeccafa669d9c39f33341d/chiang2016.pdf)
  - Impl based again based on PBRT's https://www.pbr-book.org/4ed/Reflection_Models/Scattering_from_Hair
- [x] Curve (hair, fur) rendering
  - https://github.com/mos9527/Foundation/pull/15
  - Blender `POLY` splines as standard glTF `LINES` + `_RADIUS` + `TEXCOORD_0` (via https://github.com/mos9527/Foundation-Blender-IO)
  - Import bakes Disjoint Orthogonal Triangle Strips (DOTS)    
    - See also [Path-Traced Hair Rendering in Indiana Jones and the Great Circle](https://vulkan.org/user/pages/09.events/vulkanised-2026/1145-Jiho-Choi-NVIDIA%201.pdf)
    - https://github.com/NVIDIA-RTX/RTXCR is used as a reference implementation
  - [ ] TODO Possiblity to bind strands to mesh - therefore texturing it?
  - [ ] TODO Support NV [LSS](https://developer.nvidia.com/blog/render-path-traced-hair-in-real-time-with-nvidia-geforce-rtx-50-series-gpus/)?
- [x] Path Traced Skin BSSRDF
  - Disney BSSRDF from [PBRTv3](https://github.com/mmp/pbrt-v3/blob/master/src/materials/disney.cpp)
  - Uniformly selects exitance point on scattered path via AnyHit + reservoir sampling
- [x] Transparent shadows from Area Lights
  - Environment lights and emissive objects naturally cast caustics.
  - Area lights are also added as procedural geometry into TLAS, allowing them to be hit by BSDF rays and evaluated with MIS w/ NEE.
  - Needs a large energy/firefly clamp.
  - Not done for analytical lights (Point/Directional) as they are delta distributions and cannot be hit by BSDF rays.
    - Conversion to small disk lights is feasible, or another O(N) loop to evaluate all of those inline w/o going through scene BVH
    - Not worth it nonetheless. This is merely brute-forcing.
- [x] Light BVH sampling for scene lights
  - https://github.com/mos9527/Foundation/pull/14
  - https://fileadmin.cs.lth.se/graphics/research/papers/2019/dyn_manylight/MPC19_presentation.pdf, implementation referencing https://github.com/NVIDIAGameWorks/Falcor
  - Also PBRTv4's implementation in https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling
- [x] Importance sampling Infinite Image Lights

Done:
- [Revisiting Physically Based Shading at Imageworks - Kulla & Conty 2017](https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_slides_v2.pdf)
  - [x] *Complete* Multiscatter Energy Compensation
    - See also `Scripts/LUTPrecomputeGGX.ipynb`
    - [x] Dielectrics Reflection
      - Approximate aniso materials with the same LUT
    - [x] Dielectrics Transmission
      - Implies another dimension on IOR
      - Darkening *still* noticeable at medium roughness.
        - Cycles suffer from the same issue. Try out https://github.com/mos9527/Foundation-Resources/tree/master/white-furnace
        - The slides, curiously, does not demonstrate white furnace for glass compensation
        - More bounces can somewhat remedy the issue
        - TODO Implement Heitz Random Walk ab initio?
          - https://mos9527.com/posts/foundation/pt-6-path-tracing-adventures/#random-walk-heitz-2016 
- [x] Texture sampling rate via ray differentials for primary rays
  - Easy for camera rays, not so much for ones bouncing off BSDFs 
- [x] Coat BSDF
- [x] Sheen BSDF/SGGX approx.
  - LTC Sheen "Practical Multiple-Scattering Sheen Using Linearly Transformed Cosines" https://github.com/tizian/ltc-sheen
  - ~~https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_sheen.pdf~~
- [x] PCG (Independent), Sobol Samplers

Raster
---
Unfortunately not the favourite child. Maybe one day.
- [x] IBL
- [x] Screen Space Ambient Occlusion
  - [x] [GTAO](https://www.activision.com/cdn/research/PracticalRealtimeStrategiesTRfinal.pdf)
    - Dollar store version. Doesn't even bother with denoising. Taken from Unity HDRP.
- [ ] Screen Space Reflections
- [ ] Screen Space Diffuse GI
- [ ] Light Probe Volumes
- [ ] _Really_ Speed up meshlet continuous LOD selection.
  - We're O(N). Nanite does it O (log N) via BVH

Realtime Path Tracer
---
- [ ] (Light sampler uses Progressive PT ones - TODOs omitted here)
- [ ] ReSTIR PT/DI