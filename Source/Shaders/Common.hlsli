#ifndef FOUNDATION_COMMON
#define FOUNDATION_COMMON
#include "../defines.hpp"

#define WAVE_SIZE 32 // TODO: move this to the compiler defines
#define ROOT_SIG \
    "RootFlags(SAMPLER_HEAP_DIRECTLY_INDEXED | CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)," \
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