#include "ICommon.slang"
[[SpecializationConstant]] int flags;

struct PushConstant {
    int w, h;
}
[[vk::push_constant]] PushConstant pc;

uniform UBO globalParams;
Texture2D<float4> RT0;
Texture2D<float4> RT1;
Texture2D<float4> RT2;
RWTexture2D<float4> output;

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.w || tid.y >= pc.h) return;
    float4 color0 = RT0.Load(uint3(tid,0));
    float4 color1 = RT1.Load(uint3(tid,0));
    float4 color2 = RT2.Load(uint3(tid,0));

    float3 baseColor = color0.xyz;
    uint32_t materialID = (uint)(color0.w * 255.0f) + globalParams.firstMaterial;

    float3 N, T;
    unpackTBN888(color1.xyz, N, T);
    float metallic = color1.w;
    float roughness = color2.w;
    float3 emissive = color2.xyz;

    if (flags & kGBufferViewBaseColor)
        output[tid] = float4(baseColor,1);
    else if (flags & kGBufferViewNormal)
        output[tid] = float4(N * 0.5f + 0.5f,1);
    else if (flags & kGBufferViewMaterialID) // vvv This is stupid. But needed to silence https://github.com/shader-slang/slang/issues/6947
        output[tid] = float4(unpackUnorm4x8ToFloat(hash(materialID - globalParams.firstMaterial)).xyz,1);
    else
        output[tid] = float4(baseColor + emissive,1);
}
