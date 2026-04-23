Foundation
---

Docs are available at: https://mos9527.com/Foundation

TODO (Editor)
---
- [ ] Color Science/Grading pipeline. OCIO?
- [ ] Animation (Skinning, BlendShapes...why not)

Good to have, not necessary.
- [ ] Scene Graph, instead of AoS to represent instances.

Already done.
- [x] HDR Display Output (Standard PQ OOTF+EOTF)


TODO (Path Tracer)
---
Also w/ blog post series update on:
- [ ] Light Tree ([BVH Light Sampling](https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling))
- [ ] ReSTIR
  - OG https://research.nvidia.com/sites/default/files/pubs/2020-07_Spatiotemporal-reservoir-resampling/ReSTIR.pdf
  - NV released [this (2026)](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/lin2026restirptenhanced.pdf) recently too. Lots of new tricks I've not heard of :D 
- [ ] Spatial Reuse ([SHaRC](https://github.com/NVIDIA-RTX/SHARC)?)
- [ ] Denoising (SVGF? NRD? Integrate with DLSS-RR or FSR4-RGEN?)

Some time in the future:
- [ ] Glass BSDF energy compensation 
- [ ] Volume rendering
- [ ] Skin. Random Walk SSS?
- [ ] Cluster Acceleration Structures (CLAS)
  - [DXR 2.0 will arrive sometime this year/2026](https://asawicki.info/news_1801_directx_12_news_from_gdc_2026_-_my_comments)! Once
    this is no longer NV only I'll have a shot.
  
Already done!
- [x] Transparent shadows (caustics, approximation)
  - Environment lights and emissive objects naturally cast caustics.
  - Area lights are also added as actual geometry into TLAS, allowing them to be hit by BSDF rays and evaluated with MIS w/ NEE.
  - Cheap approximation still - unbiased, yes. But very slow to converge. Crank up the fireflies!
  - Not done for analytical lights (Point/Directional) as they are delta distributions and cannot be hit by BSDF rays.
    - Conversion to small disk lights is feasible, or another O(N) loop to evaluate all of those inline w/o going through scene BVH
- [x] Alias Sampling scene lights
- [x] PCG (Independent), Sobol Samplers
- [x] Importance sampling Infinite Image Lights

TODO (Raster)
---
Unfortunately not the favourite child. Maybe one day.
- [ ] Speed up meshlet continuous LOD selection.
  - We're O(N). Nanite does it O (log N) via BVH
- [ ] IBL
- [ ] LTC Area lights
- [ ] Screen Space Diffuse GI (SSGI)
- [ ] Screen Space Reflections 
- [ ] HBAO

Demo (Path Tracer)
---
Some scenes used here can be found at [Foundation-Resources](https://github.com/mos9527/Foundation-Resources)

> "Stormtrooper Star Wars VII by ScottGraham" from [Blend Swap](https://www.blendswap.com/blend/13953)

<img  width="1200" alt="image" src="https://github.com/user-attachments/assets/793b95f1-fae1-40a9-ae89-50da476d49bb" />

> "Archinteriors Vol. 48, Scene 08 by Evermotion" from [Evermotion](https://evermotion.org/shop/show_product/archinteriors-vol-48/14307)

<img width="1200" height="675" alt="image" src="https://github.com/user-attachments/assets/5f4ea058-c51a-4011-a449-a0a1d4cfcfac" />

> "Dining room by MaTTeSr" from [Blend Swap](https://www.blendswap.com/blend/18762)

<img width="1198" height="1024" alt="image" src="https://github.com/user-attachments/assets/a47bd8d4-f394-43f7-9d5a-336e8181151f" />


Links
---

https://mos9527.com/posts/foundation/pt-1-mesh-shader-continous-lod/

https://mos9527.com/posts/foundation/pt-2-gpu-driven-pipeline-with-culling/

https://mos9527.com/posts/foundation/pt-3-profiler-and-wave-intrinsics/

https://mos9527.com/posts/foundation/pt-4-mesh-quantization/

https://mos9527.com/posts/foundation/pt-5-texture-compression-and-gbuffer/

https://mos9527.com/posts/foundation/pt-6-path-tracing-adventures/
