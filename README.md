Foundation
---

Docs are available at: https://mos9527.com/Foundation

TODO (Editor)
---
- [ ] Color Science/Grading pipeline. OCIO?
- [ ] Animation (Skinning, BlendShapes...why not)

Good to have, not necessary.
- [ ] Scene Graph, instead of AoS to represent instances.

TODO (Path Tracer)
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

Some time in the future:
- [ ] Glass BSDF energy compensation 
- [ ] Cluster Acceleration Structures (CLAS)
  - [DXR 2.0 will arrive sometime this year/2026](https://asawicki.info/news_1801_directx_12_news_from_gdc_2026_-_my_comments)! Once
    this is no longer NV only I'll have a shot.
  
Done, awaiting Blog Update:
- [x] LUT-based color management implementation
  - Linear BT709 Scene Space (D65), converted to D60 via [Bradford CAT](http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html) and encoded as [AP1](https://docs.acescentral.com/encodings/aces2065-1/#transfer-function) 
  - Transform then encodes as [ACEScct](https://docs.acescentral.com/encodings/acescct/#encoding-function)
  - Rest of the transform handled by LUTs in ACEScct log space, incl. EOTF. See `Scripts/OCIOBakeLUTs.py`
  - Both SDR/HDR displays accounted for. Again, refer to the script for exact transforms used.
- [x] Hair Shading
  - A Practical and Controllable Hair and Fur Model for Production Path Tracing [Chiang et al. 2016]
  - Impl based again based on PBRT's https://www.pbr-book.org/4ed/Reflection_Models/Scattering_from_Hair
- [x] Curve (hair, fur) rendering
  - Beizer Segments via [De Casteljau](https://zh.wikipedia.org/wiki/%E5%BE%B7%E5%8D%A1%E6%96%AF%E7%89%B9%E9%87%8C%E5%A5%A5%E7%AE%97%E6%B3%95), a la PBRT's [Curves](https://www.pbr-book.org/4ed/Shapes/Curves#Curve)
  - Intersected as flat cross-sections with TBN that 'makes it look like a swept cylinder'
  - Procedurally traced, data exchange done via our own Blender IO (https://github.com/mos9527/Foundation-Blender-IO)
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
