#ifndef HLSL_CPP_STRUCTS
#define HLSL_CPP_STRUCTS
#ifdef __cplusplus
#include "../../pch.hpp"
#endif
typedef uint meshlet_triangles_swizzled;
struct meshlet_bounds_swizzled {
    /* bounding sphere, useful for frustum and occlusion culling */
    float center[3];
    float radius;

    /* normal cone, useful for backface culling */
    float cone_apex[3];
    float cone_axis[3];
    float cone_cutoff; /* = cos(angle/2) */
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
struct GeometryGPULod
{
    uint index_offset;
    uint index_count;
    uint meshlet_offset;
    uint meshlet_bound_offset;
    uint meshlet_vertices_offset;
    uint meshlet_triangles_offset;
    uint meshlet_count;
};
struct GeometryData
{    
    uint heap_index;

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

struct InstanceData {
	uint instance_index;
	uint geometry_index;
	uint material_index;

    float4 aabb_sphere_center_radius;
	matrix transform;
};

struct CameraData {
    float4 position;
    float4 direction;
    float4 FovAspectNearZFarZ;
    float4 clipPlanes[6];

    matrix view;
    matrix projection;
    matrix viewProjection;
};
#endif