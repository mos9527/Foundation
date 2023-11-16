#ifndef FOUNDATION_DEFINES
#define FOUNDATION_DEFINES

#define INVERSE_Z

#define RHI_USE_D3D12
#define RHI_D3D12_USE_AGILITY
#define RHI_USE_D3D12_MESH_SHADER
#define RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT 2
#define RHI_DEFAULT_DIRECT_COMMAND_ALLOCATOR_COUNT RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT
#define RHI_DEFAULT_COPY_COMMAND_ALLOCATOR_COUNT 1
#define RHI_DEFAULT_COMPUTE_COMMAND_ALLOCATOR_COUNT 1

#define ALLOC_SIZE_DESCHEAP 0xffff
#define ALLOC_SIZE_SHADER_VISIBLE_DESCHEAP 2048
#define MAX_INSTANCE_COUNT 0xffff
#define DISPATCH_GROUP_COUNT 0xffff

#define MESHLET_MAX_VERTICES 64u // https://developer.nvidia.com/blog/introduction-turing-mecacsh-shaders/
#define MESHLET_MAX_PRIMITIVES 124u // 4b aligned

#define MAX_LOD_COUNT 8
#define LOD_GET_RATIO(lod) ((float)(MAX_LOD_COUNT - lod) / MAX_LOD_COUNT)

#define THREADS_PER_WAVE 32 // Assumes availability of wave size of 32 threads
// Pre-defined threadgroup sizes for AS & MS stages
#define MAX(x, y) (x > y ? x : y)
#define ROUNDUP(x, y) ((x + y - 1) & ~(y - 1))
#define AS_GROUP_SIZE THREADS_PER_WAVE
#define MS_GROUP_SIZE ROUNDUP(MAX(MESHLET_MAX_VERTICES, MESHLET_MAX_PRIMITIVES), THREADS_PER_WAVE)
#endif
