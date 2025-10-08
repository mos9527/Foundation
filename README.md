Foundation
---
Docs are available at: https://mos9527.com/Foundation

## TODO
- ~ModelViewer | Meshlet generations & rendering~
- ~~ModelViewer | ImGUI integration~~
- ModelViewer | !! Mem leak !!
- ModelViewer | 2-phase occlusion culling with HZB
- ModelViewer | AS building & inline RT usage
- ModelViewer | Scene graph
- ModelViewer | glTF spec compliance 
## VVL TODO
### References
- https://www.lunarg.com/wp-content/uploads/2021/08/Vulkan-Synchronization-SIGGRAPH-2021.pdf   
### TODO
- ~~!! SYNC HAZARD with FillBuffer -> CopyBuffer !!~~ Fixed by 
- ~~!! SYNC HAZARD Present after write !!~~
    False positive. Fixed by https://github.com/KhronosGroup/Vulkan-ValidationLayers/pull/10680
- ~~vkCreatePipelineLayout(): pCreateInfo->pPushConstantRanges[0].size (144) plus offset (0) is greater than maxPushConstantSize (128).~~
    Go over 128 on AMD or 256 or NVIDIA and you'll be an artist - or get a epilepsy episode. Don't do that.
- ModelViewer | 32-bit Depth on NVIDIA
```
Validation Performance Warning: [ BestPractices-NVIDIA-CreateImage-Depth32Format ] | MessageID = 0xf00e92a8
[AppName: Vulkan RHI] vkCreateImage(): [NVIDIA] Trying to create an image with a 32-bit depth format. Use VK_FORMAT_D24_UNORM_S8_UINT or VK_FORMAT_D16_UNORM instead, unless the extra precision is needed.
```
- ~~AMD wave size/compute size~~
    Prefer multiples of 64 on AMD, 32 on NVIDIA. Using a lcm of 64 could be a good compromise.
- Best practices
```
[2025-10-07 11:16:13.267] [VkDebugLayer] [warning] [AppName: Vulkan RHI] vkQueueSubmit(): pSubmits[0].pWaitDstStageMask[0] using VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT

Validation Performance Warning: [ BestPractices-NVIDIA-CreateImage-TilingLinear ] | MessageID = 0xdf3a8534
[AppName: Vulkan RHI] vkCreateImage(): [NVIDIA] Trying to create an image with tiling VK_IMAGE_TILING_LINEAR. Use VK_IMAGE_TILING_OPTIMAL instead.

[2025-10-07 11:16:14.417] [VkDebugLayer] [warning] [AppName: Vulkan RHI] vkCmdBindPipeline(): [NVIDIA] Avoid switching between pipelines with and without tessellation, geometry, task, and/or mesh shaders. Group draw calls using these shader stages together.

Validation Performance Warning: [ BestPractices-NVIDIA-BindPipeline-SwitchTessGeometryMesh ] | MessageID = 0xce1a412d
[AppName: Vulkan RHI] vkCmdBindPipeline(): [NVIDIA] Avoid switching between pipelines with and without tessellation, geometry, task, and/or mesh shaders. Group draw calls using these shader stages together.
Objects: 1
    [0] VkCommandBuffer 0xb4cfa2f280[Graphics List 1]
```
- ~~Extensions already in Core~~