Foundation
===

Docs are available at: https://mos9527.com/Foundation

Blog Posts 
---
- https://mos9527.com/posts/foundation/pt-1-mesh-shader-continous-lod/
- https://mos9527.com/posts/foundation/pt-2-gpu-driven-pipeline-with-culling/
- https://mos9527.com/posts/foundation/pt-3-profiler-and-wave-intrinsics/
- https://mos9527.com/posts/foundation/pt-4-mesh-quantization/
- https://mos9527.com/posts/foundation/pt-5-texture-compression-and-gbuffer/
- https://mos9527.com/posts/foundation/pt-6-path-tracing-adventures/

- [Foundation-Resources](https://github.com/mos9527/Foundation-Resources)

Framework
---
- RHI Backends
  - [x] Vulkan
    - [x] Desktop (Windows & Linux) Probably the only platform we truly care about
    - [ ] Mobile.      
      - _Try_ getting examples to work there. Seems (very much not) fun...
      - Almost no device supports the full RT pipeline
      - Newer Adrenos support [Mesh Shaders](https://docs.qualcomm.com/doc/80-78185-2/topic/mobile_best_practices.html#panel-1-1-1)        
  - [-] Metal
    - Loosely speaking - Apple GPUs are all tilers. So they are all categorically mobile.
    - There are Vulkan-on-Metal layers, which we do work with - albeit with lots of limitations.            
      - Our Examples that don't use unsupported features work OOTB (macOS)            
      - Raster looks good with up-to-date Mesh Shader support. Practically _no_ VK compat layers support them yet. [2026/05/10]
      - Raytracing - No native SBTs. No SER. Metal can do inline queries only.  
      - [ ] TODO iOS builds?
  - [x] Lavapipe
    - https://www.vulkan.org/user/pages/09.events/vulkanised-2025/T5-Lucas-Fryzek-Igalia.pdf
    - Small Guide on Windows usage:
      - Get build from https://github.com/pal1000/mesa-dist-win/releases
      - Set environment variable VK_ADD_DRIVER_FILES to fullpath pointing to `lvp_icd.x86_64.json` (per [Driver Discovery](https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md#driver-discovery))
      - Run stuff from the shell.

Editor
---
- [x] **Correct** SDR&HDR Color Pipeline
  - Linear BT709 Scene Space (D65), converted to D60 via [Bradford CAT](http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html) and encoded as [AP1](https://docs.acescentral.com/encodings/aces2065-1/#transfer-function) 
  - Transform then encodes as [ACEScct](https://docs.acescentral.com/encodings/acescct/#encoding-function)
  - Rest of the transform handled by LUTs in ACEScct log space, incl. to display EOTF. See `Scripts/OCIOBakeLUTs.py`
  - Blender OCIO Config used to generated LUTs for SDR/HDR ACES1.3/2.0/AgX/Standard (sRGB. v. PQ) transforms
    - See also https://docs.blender.org/manual/en/latest/render/color_management/  
    - Obtains 1-to-1 matching output :)  
- [ ] Animation (Skinning, BlendShapes...why not)

Good to have, not necessary.
- [ ] Scene Graph, instead of AoS to represent instances.

Path Tracer
---
Also w/ blog post series update on:
- [ ] Denoising
- [ ] Light Tree ([BVH Light Sampling](https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling))
- [ ] ReSTIR Spatial + Temporal Reuse
- [ ] Spatial Reuse ([SHaRC](https://github.com/NVIDIA-RTX/SHARC)?)
- [ ] MNEE (Manifold NEE) for *much* better caustics convergence
  - https://jo.dreggn.org/home/2015_mnee_talk.pdf
- [ ] Texture sampling rate via ray differentials
  - Easy for camera rays, not so much for ones bouncing off BSDFs
- [ ] Volume rendering
- [Revisiting Physically Based Shading at Imageworks - Kulla & Conty 2017](https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_slides_v2.pdf)
  - [ ] *Complete* Multiscatter Energy Compensation 
    - See also `Scripts/LUTPrecomputeGGX.ipynb`
    - [x] Dielectrics Reflection
      - [ ] Approximate aniso materials with the same LUT    
    - [ ] Dielectrics Transmission
      - Implies another dimension on IOR
  - [ ] Coat BSDF
  - [ ] Sheen BSDF
    - https://blog.selfshadow.com/publications/s2017-shading-course/imageworks/s2017_pbs_imageworks_sheen.pdf

Done, awaiting Blog Update:
- [x] Hair Shading
  - [A Practical and Controllable Hair and Fur Model for Production Path Tracing [Chiang et al. 2016]](https://sci-hub.sg/storage/2024/5766/a1c3a0a1d0aeccafa669d9c39f33341d/chiang2016.pdf)
  - Impl based again based on PBRT's https://www.pbr-book.org/4ed/Reflection_Models/Scattering_from_Hair
- [x] Curve (hair, fur) rendering
  - Beizer Segments via [De Casteljau](https://zh.wikipedia.org/wiki/%E5%BE%B7%E5%8D%A1%E6%96%AF%E7%89%B9%E9%87%8C%E5%A5%A5%E7%AE%97%E6%B3%95), a la PBRT's [Curves](https://www.pbr-book.org/4ed/Shapes/Curves#Curve)
  - Intersected as flat cross-sections with TBN that 'makes it look like a swept cylinder'
  - Procedurally traced, data exchange done via our own Blender IO (https://github.com/mos9527/Foundation-Blender-IO)
  - [ ] TODO Possiblity to bind strands to mesh - therefore texturing it?
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
- [x] Alias Sampling scene lights
- [x] PCG (Independent), Sobol Samplers
- [x] Importance sampling Infinite Image Lights

Raster
---
Unfortunately not the favourite child. Maybe one day.
- [ ] _Really_ Speed up meshlet continuous LOD selection.
  - We're O(N). Nanite does it O (log N) via BVH 
- [ ] IBL
- [ ] Screen Space Diffuse GI
- [ ] Screen Space Reflections 
- [ ] Screen Space Ambient Occlusion
- [ ] Light Probe Volumes
