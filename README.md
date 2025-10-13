Foundation
---
Docs are available at: https://mos9527.com/Foundation

## TODO
- ~ModelViewer | Meshlet generations & rendering~
- ~~ModelViewer | ImGUI integration~~
- ModelViewer | 2-phase occlusion culling with HZB
- ModelViewer | AS building & inline RT usage
- ModelViewer | Scene graph
- ModelViewer | glTF import

## FIXME
- !! `TexturePool` threading hazards

```
[2025-10-13 11:00:19.069] [VkDebugLayer] [error] vkAllocateDescriptorSets(): THREADING ERROR : object of type VkDescriptorPool is simultaneously used in current thread 136336945788608 and thread 136336799758016
[2025-10-13 11:00:19.069] [VkDebugLayer] [error] vkAllocateDescriptorSets(): THREADING ERROR : object of type VkDescriptorPool is simultaneously used in current thread 136336986015424 and thread 136336799758016
[2025-10-13 11:00:19.069] [VkDebugLayer] [error] vkAllocateDescriptorSets(): THREADING ERROR : object of type VkDescriptorPool is simultaneously used in current thread 136336770176704 and thread 136336945788608
```
