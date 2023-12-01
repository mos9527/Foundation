#include "Common.hlsli"
// RTR4 p663
cbuffer BlendConstants : register(b0, space0)
{
    uint width;
    uint height;
    uint framebufferUavHandle;
    uint depthUavHandle;
}
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b1, space0);
// Sobel edge kernels
const static float3x3 sx =
{
    1, 0, -1,
    2, 0, -2,
    1, 0, -1
};
const static float3x3 sy =
{
    1, 2, 1,
    0, 0, 0,
    -1, -2, -1
};

[numthreads(RENDERER_FULLSCREEN_THREADS, RENDERER_FULLSCREEN_THREADS, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
    if (any(DTid > uint2(width - 1, height - 1)) || any(DTid < uint2(1, 1))) // Prevent OOB
        return;
    Texture2D<float4> depthTex = ResourceDescriptorHeap[depthUavHandle];
    RWTexture2D<float4> framebuffer = ResourceDescriptorHeap[framebufferUavHandle];   
    float3x3 A;
    for (uint x = 0; x < 3; x++)
    {
        for (uint y = 0; y < 3; y++)
        {
            A[x][y] = clipZ2ViewZ(depthTex[DTid + int2(x - 1, y - 1)].r, g_SceneGlobals.camera.nearZ, g_SceneGlobals.camera.farZ);
        }
    }
    float Gx = dot(sx[0], A[0]) + dot(sx[1], A[1]) + dot(sx[2], A[2]);
    float Gy = dot(sy[0], A[0]) + dot(sy[1], A[1]) + dot(sy[2], A[2]);
    float g = sqrt(Gx * Gx + Gy * Gy);
    if (g > g_SceneGlobals.edgeThreshold)
        framebuffer[DTid] = float4(g_SceneGlobals.edgeColor,1);
}