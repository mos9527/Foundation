#pragma once
/* ENGINE VERSION */
#define FOUNDATION_VERSION_MAJOR 0
#define FOUNDATION_VERSION_MINOR 0
#define FOUNDATION_VERSION_PATCH 0
#define FOUNDATION_VERSION_SPECIAL "/poc"
// https://stackoverflow.com/a/6990647
#define QU(x) #x
#define QUH(x) QU(x)
#define FOUNDATION_VERSION_STRING QUH(FOUNDATION_VERSION_MAJOR) "." QUH(FOUNDATION_VERSION_MINOR) "." QUH(FOUNDATION_VERSION_PATCH) "" FOUNDATION_VERSION_SPECIAL

/* RHI */
#define RHI_USE_D3D12
#define RHI_D3D12_USE_AGILITY
// #define RHI_USE_MESH_SHADER

#define RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT 3
#define RHI_DEFAULT_DIRECT_COMMAND_ALLOCATOR_COUNT RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT
#define RHI_DEFAULT_COPY_COMMAND_ALLOCATOR_COUNT 1
#define RHI_DEFAULT_COMPUTE_COMMAND_ALLOCATOR_COUNT 1
#define RHI_ALLOC_SIZE_DESCHEAP 0xffff
#define RHI_ALLOC_SIZE_SHADER_VISIBLE_DESCHEAP 2048
#define RHI_ZERO_BUFFER_SIZE 0xffff

/* EDITOR / EDITOR SHADER DEFINES */
#define IMGUI_ENABLED
#define INVERSE_Z

#define INVALID_HEAP_HANDLE ((uint)-1)

#define MAX_INSTANCE_COUNT 0xffff
#define MAX_LIGHT_COUNT 16
#define MAX_MATERIAL_COUNT 0xffff

#define MESHLET_MAX_VERTICES 64u // https://developer.nvidia.com/blog/introduction-turing-mecacsh-shaders/
#define MESHLET_MAX_PRIMITIVES 124u // 4b aligned
#define MAX_LOD_COUNT 4
#define THREADS_PER_WAVE 64

#define RENDERER_INSTANCE_CULL_THREADS THREADS_PER_WAVE
#define RENDERER_FULLSCREEN_THREADS 8 // 8 * 8 -> THREADS_PER_WAVE
#define DEFBIT(I) (1 << I)
#define FRAME_FLAG_DEBUG_VIEW_ALBEDO DEFBIT(0)
#define FRAME_FLAG_DEBUG_VIEW_LOD DEFBIT(1)
#define FRAME_FLAG_WIREFRAME DEFBIT(2)
#define FRAME_FLAG_FRUSTUM_CULL DEFBIT(3)
#define FRAME_FLAG_OCCLUSION_CULL DEFBIT(4)

#define INSTANCE_FLAG_OCCLUDER DEFBIT(0) // occludes other geometry
#define INSTANCE_FLAG_OCCLUDEE DEFBIT(1) // can be occluded
#define INSTANCE_FLAG_INVISIBLE DEFBIT(2)
#define INSTANCE_FLAG_DRAW_BOUNDS DEFBIT(3)
#define INSTANCE_FLAG_TRANSPARENCY DEFBIT(4)

#define FRAME_FLAG_DEFAULT (FRAME_FLAG_FRUSTUM_CULL | FRAME_FLAG_OCCLUSION_CULL)
