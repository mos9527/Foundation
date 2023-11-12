#ifndef FOUNDATION_COMMON
#define FOUNDATION_COMMON
#include "../defines.hpp"
#include "../Renderer/Renderer/Structs.hpp"
#define WAVE_SIZE 32 // TODO: move this to the compiler defines
#define ROOT_SIG \
    "RootFlags(SAMPLER_HEAP_DIRECTLY_INDEXED | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
    "RootConstants(b0, num32BitConstants = 3)," \
    "RootConstants(b1, num32BitConstants = 2)"
struct BufferIndices
{
    uint instanceBufferIndex;
    uint geometryIndexBufferIndex;
    uint cameraIndex;
};
ConstantBuffer<BufferIndices> g_bufferIndices : register(b0);
struct DispatchConstants
{
    uint dispatch_offset;
    uint dispatch_count;
};
ConstantBuffer<DispatchConstants> g_dispatchData : register(b1);
inline uint3 triangles_unpack(uint primitive)
{
    return uint3(primitive & 0x3FF, (primitive >> 10) & 0x3FF, (primitive >> 20) & 0x3FF);
}

struct AS2MSPayload
{
    uint indices[THREADS_PER_WAVE];
};

struct Vertex
{
    float4 position : SV_Position;
    float3 positionWs : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;    
};
bool IsVisible(CameraData camera, float4 aabb_sphere)
{
    float4 P = float4(aabb_sphere.xyz, 1.0f);
    float r = aabb_sphere.w;
    for (uint i = 0; i < 6; i++)
    {
        if (dot(P, camera.clipPlanes[i]) < -r)
            return false;
    }
    return true;
}

float ComputeLOD(CameraData camera, float4 aabb_sphere)
{
    float3 v = aabb_sphere.xyz - camera.position.xyz;
    float r = aabb_sphere.w;
    float l0 = rcp(tan(camera.FovAspectNearZFarZ.x / 2)) * r; // min depth where sphere completely fills the viewport
    // r / l0 = r' / depth
    // ratio = r / r' -> l0 / depth
    float ratio = l0 / length(v);
    ratio = min(ratio, 1.0f);
    return ratio;
}
#endif