#include "Common.hlsli"
#include "Math.hlsli"
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b0, space0);
SamplerState g_Sampler : register(s0, space0);
struct PSInput
{
    float4 Pos : SV_POSITION; // Pixel coordinates
    float3 UVW : TEXCOORD0; // UVW for cube    
};

PSInput vs_main(float3 In : POSITION)
{
    PSInput Output;
    matrix View = g_SceneGlobals.camera.view;
    float4x4 ViewNoTranslation =
    {
        View._11, View._12, View._13, 0,
        View._21, View._22, View._23, 0,
        View._31, View._32, View._33, 0,
        0, 0, 0, 0
    };
    matrix ViewProjection = mul(ViewNoTranslation, g_SceneGlobals.camera.projection);
    Output.Pos = mul(float4(In.xyz, 1.0f), ViewProjection); // ensure z/w=1
#ifdef INVERSE_Z
    Output.Pos.z = 0;
#else
    Output.Pos.z = Output.Pos.w;
#endif
    Output.UVW = In;    
    return Output;
}
float4 ps_main(PSInput input) : SV_Target
{
    if (g_SceneGlobals.probe.enabled)
    {
        TextureCube cube = ResourceDescriptorHeap[g_SceneGlobals.probe.radianceHeapIndex];
        return cube.SampleLevel(g_Sampler, input.UVW,g_SceneGlobals.probe.skyboxLod);
    }
    else
    {
        return float4(0,0,0,1);        
    }
}
