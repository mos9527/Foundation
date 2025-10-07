Foundation
---
Docs are available at: https://mos9527.com/Foundation

## TODO
- ~ModelViewer | Meshlet generations & rendering~
- ~~ModelViewer | ImGUI integration~~
- ModelViewer | 2-phase occlusion culling with HZB
- ModelViewer | AS building & inline RT usage
- ModelViewer | Scene graph
- ModelViewer | glTF spec compliance 

## VVL TODO
### References
- https://www.lunarg.com/wp-content/uploads/2021/08/Vulkan-Synchronization-SIGGRAPH-2021.pdf   
### TODO
- !! SYNC HAZARD with FillBuffer -> CopyBuffer !!
```
Validation Error: [ SYNC-HAZARD-WRITE-AFTER-WRITE ] | MessageID = 0x5c0ec5d6
[AppName: Vulkan RHI] vkCmdCopyBuffer(): WRITE_AFTER_WRITE hazard detected. vkCmdCopyBuffer writes to VkBuffer 0x160000000016[GPUScene Const], which was previously written by vkCmdFillBuffer[Const Buffer::Staging Buffer Updates]. 
No sufficient synchronization is present to ensure that a write (VK_ACCESS_2_TRANSFER_WRITE_BIT) at VK_PIPELINE_STAGE_2_COPY_BIT does not conflict with a prior write of the same type at VK_PIPELINE_STAGE_2_CLEAR_BIT.
Buffer copy region 0: {
  offset = 0,
  size = 46538
}
[Extra properties]
message_type = BufferCopyError
hazard_type = WRITE_AFTER_WRITE
access = VK_PIPELINE_STAGE_2_COPY_BIT(VK_ACCESS_2_TRANSFER_WRITE_BIT)
prior_access = VK_PIPELINE_STAGE_2_CLEAR_BIT(VK_ACCESS_2_TRANSFER_WRITE_BIT)
write_barriers = 0
command = vkCmdCopyBuffer
prior_command = vkCmdFillBuffer
prior_debug_region = Const Buffer::Staging Buffer Updates
region_index = 0
seq_no = 2
reset_no = 0
Objects: 2
    [0] VkCommandBuffer 0xb4cf9acdb0[Graphics List 0]
    [1] VkBuffer 0x160000000016[GPUScene Const]
```
- !! SYNC HAZARD Present after write !!
```
[2025-10-07 11:16:13.332] [VkDebugLayer] [warning] [AppName: Vulkan RHI] vkCmdPipelineBarrier(): pImageMemoryBarriers[0] image is VkImage 0xc000000000c[Backbuffer of Swap 0] and accessMask is VK_ACCESS_2_MEMORY_READ_BIT, but for layout VK_IMAGE_LAYOUT_PRESENT_SRC_KHR expected accessMask are VkAccessFlags2(0).
Validation Error: [ SYNC-HAZARD-PRESENT-AFTER-WRITE ] | MessageID = 0xe17ab4ae
[AppName: Vulkan RHI] vkQueuePresentKHR(): PRESENT_AFTER_WRITE hazard detected. vkQueuePresentKHR presents swapchain image 0 (VkImage 0xc000000000c[Backbuffer of Swap 0] from VkSwapchainKHR 0xb000000000b), which was previously written during an image layout transition initiated by vkCmdEndRenderPass. 
No sufficient synchronization is present to ensure that a swapchain present operation does not conflict with a prior layout transition.
[Extra properties]
message_type = PresentError
hazard_type = PRESENT_AFTER_WRITE
access = SYNC_PRESENT_ENGINE_SYNCVAL_PRESENT_PRESENTED_SYNCVAL
prior_access = SYNC_IMAGE_LAYOUT_TRANSITION
write_barriers = 0
command = vkQueuePresentKHR
prior_command = vkCmdEndRenderPass
submit_index = 2
batch_index = 0
swapchain_index = 0
seq_no = 5
reset_no = 0
batch_tag = 42
Objects: 3
    [0] VkQueue 0xb4ce868710[Graphics Queue]
    [1] VkSwapchainKHR 0xb000000000b
    [2] VkImage 0xc000000000c[Backbuffer of Swap 0]
```
- High `VkFence` count - maybe fencing for compute is not needed?
```
[2025-10-07 11:16:13.036] [Renderer] [info] [Renderer.cpp:935] Swapchain uses 3 back buffers
Validation Performance Warning: [ BestPractices-SyncObjects-HighNumberOfFences ] | MessageID = 0xa9f4ff68
[AppName: Vulkan RHI] vkCreateFence(): [AMD]  High number of VkFence objects created. 4 created, but recommended max is 3. Minimize the amount of CPU-GPU synchronization that is used. Each fence has a CPU and GPU overhead cost with it.

[2025-10-07 11:16:13.037] [VkDebugLayer] [warning] [AppName: Vulkan RHI] vkCreateFence(): [AMD]  High number of VkFence objects created. 4 created, but recommended max is 3. Minimize the amount of CPU-GPU synchronization that is used. Each fence has a CPU and GPU overhead cost with it.
Validation Performance Warning: [ BestPractices-SyncObjects-HighNumberOfFences ] | MessageID = 0xa9f4ff68
[AppName: Vulkan RHI] vkCreateFence(): [AMD] [NVIDIA] High number of VkFence objects created. 5 created, but recommended max is 3. Minimize the amount of CPU-GPU synchronization that is used. Each fence has a CPU and GPU overhead cost with it.

[2025-10-07 11:16:13.037] [VkDebugLayer] [warning] [AppName: Vulkan RHI] vkCreateFence(): [AMD] [NVIDIA] High number of VkFence objects created. 5 created, but recommended max is 3. Minimize the amount of CPU-GPU synchronization that is used. Each fence has a CPU and GPU overhead cost with it.
[2025-10-07 11:16:13.039] [Renderer] [debug] [Renderer.cpp:475] ** Render Graph GraphViz **

```
- 32-bit Depth on NVIDIA
```
Validation Performance Warning: [ BestPractices-NVIDIA-CreateImage-Depth32Format ] | MessageID = 0xf00e92a8
[AppName: Vulkan RHI] vkCreateImage(): [NVIDIA] Trying to create an image with a 32-bit depth format. Use VK_FORMAT_D24_UNORM_S8_UINT or VK_FORMAT_D16_UNORM instead, unless the extra precision is needed.
```
- AMD wave size/compute size
```
Validation Performance Warning: [ BestPractices-AMD-LocalWorkgroup-Multiple64 ] | MessageID = 0x85f09c61
[AppName: Vulkan RHI] vkCreateComputePipelines(): pCreateInfos[0].stage [AMD] compute shader with work group dimensions (32, 1, 1), workgroup size (32), is not a multiple of 64. Make the workgroup size a multiple of 64 to obtain best performance across all AMD GPU generations.
```
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