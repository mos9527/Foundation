#ifndef FOUNDATION_DEFINES
#define FOUNDATION_DEFINES

#define IMGUI_ENABLED

#define RHI_USE_D3D12
#define RHI_D3D12_USE_AGILITY
// #define RHI_USE_D3D12_MESH_SHADER
#define RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT 2
#define RHI_DEFAULT_DIRECT_COMMAND_ALLOCATOR_COUNT RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT
#define RHI_DEFAULT_COPY_COMMAND_ALLOCATOR_COUNT 1
#define RHI_DEFAULT_COMPUTE_COMMAND_ALLOCATOR_COUNT 1

#define ALLOC_SIZE_DESCHEAP 0xffff
#define ALLOC_SIZE_SHADER_VISIBLE_DESCHEAP 2048

#define AS_GROUP_SIZE THREADS_PER_WAVE
#define MS_GROUP_SIZE ROUNDUP(MAX(MESHLET_MAX_VERTICES, MESHLET_MAX_PRIMITIVES), THREADS_PER_WAVE)
#endif
