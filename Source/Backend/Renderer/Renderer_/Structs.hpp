#ifndef HLSL_CPP_STRUCTS
#define HLSL_CPP_STRUCTS
#ifdef __cplusplus
#include "../../pch.hpp"
#pragma pack(push,1)
#endif
typedef uint meshlet_triangles_swizzled;
struct meshlet
{    
    uint vertex_offset;
    uint vertex_count;
    
    uint triangle_offset;
    uint triangle_count;

    /* bounding sphere, useful for frustum and occlusion culling */
    float center[3];
    float radius;

    /* normal cone, useful for backface culling */
    float cone_apex[3];
    uint cone_axis_cutoff;
};
struct geometry_lod
{
    uint index_offset;
    uint index_count;
    uint meshlet_offset;
    uint meshlet_count;
};
struct vertex_static {
    float3 position;
    float3 normal;
    float3 tangent;
    float2 uv;
};
struct GeometryBufferOffsets
{
    uint vertex_offset;
    uint vertex_count;

    geometry_lod lods[LOD_COUNT];
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
#ifdef __cplusplus
#pragma pack(pop)
#endif
#endif
