#ifndef FOUNDATION_DEFINES
#define FOUNDATION_DEFINES
#define RHI_USE_D3D12
#define RHI_D3D12_USE_AGILITY

#define ALLOC_SIZE_STORE_DESCHEAP 32768
#define ALLOC_SIZE_BOUND_DESCHEAP 1024

#define GEO_MAX_NUM_GEOS 0xffff
#define TEX_MAX_NUM_TEXS 0xffff

#define MESHLET_MAX_VERTICES 64u // https://developer.nvidia.com/blog/introduction-turing-mecacsh-shaders/
#define MESHLET_MAX_PRIMITIVES 124u // 4b aligned

#define LOD_COUNT 8
#define LOD_GET_RATIO(lod) ((float)lod / LOD_COUNT)

#define THREADS_PER_WAVE 32 // Assumes availability of wave size of 32 threads
// Pre-defined threadgroup sizes for AS & MS stages
#define MAX(x, y) (x > y ? x : y)
#define ROUNDUP(x, y) ((x + y - 1) & ~(y - 1))
#define AS_GROUP_SIZE THREADS_PER_WAVE
#define MS_GROUP_SIZE ROUNDUP(MAX(MESHLET_MAX_VERTICES, MESHLET_MAX_PRIMITIVES), THREADS_PER_WAVE)
#endif
