#ifndef FOUNDATION_COMMON
#define FOUNDATION_COMMON
#include "../defines.hpp"

#define WAVE_SIZE 32 // TODO: move this to the compiler defines
#define ROOT_SIG \
    "RootConstants(b0, num32BitConstants = 1)"
struct test_constants
{
    uint geometryHandleBufferIndex;
};
ConstantBuffer<test_constants> g_constants : register(b0);

typedef uint meshlet_triangles_swizzled;
inline uint3 triangles_unpack(uint primitive)
{
    return uint3(primitive & 0x3FF, (primitive >> 10) & 0x3FF, (primitive >> 20) & 0x3FF);
}
struct meshlet_bounds_swizzled {
	/* bounding sphere, useful for frustum and occlusion culling */
	float center[3];
	float radius;

	/* normal cone, useful for backface culling */
	float cone_apex[3];
	float cone_axis[3];
	float cone_cutoff; /* = cos(angle/2) */
};
struct GeometryGPULod
{
    uint meshlet_offset;
    uint meshlet_bound_offset;
    uint meshlet_vertices_offset;
    uint meshlet_triangles_offset;
    uint meshlet_count;
};
struct GeometryGPUHandle
{
    uint manager_handle;
    uint heap_handle;

    uint position_offset;
    uint position_count;

    uint normal_offset;
    uint normal_count;

    uint tangent_offset;
    uint tangent_count;

    uint uv_offset;
    uint uv_count;
		
    GeometryGPULod lods[LOD_COUNT];
};
struct meshlet
{
	/* offsets within meshlet_vertices and meshlet_triangles arrays with meshlet data */
    uint vertex_offset;
    uint triangle_offset;

	/* number of vertices and triangles used in the meshlet; data is stored in consecutive range defined by offset and count */
    uint vertex_count;
    uint triangle_count;
};

struct as_ms_payload
{
    uint null;
};

struct ms_ps_payload
{
    float4 position : SV_Position;
    float3 positionWs : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;    
};
#endif