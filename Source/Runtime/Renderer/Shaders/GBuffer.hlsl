//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************
#include "Shared.h"

cbuffer IndirectData : register(b0, space0)
{
    uint g_MeshIndex;
};
ConstantBuffer<SceneGlobals> g_SceneGlobals : register(b1, space0);
StructuredBuffer<SceneMeshInstance> g_SceneMeshInstances : register(t0, space0);
StructuredBuffer<SceneMaterial> g_Materials : register(t1, space0);
SamplerState g_Sampler : register(s0, space0);
struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;
};
struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

PSInput vs_main(VSInput vertex)
{
    PSInput result;
    SceneMeshInstance mesh = g_SceneMeshInstances[g_MeshIndex];
    result.position = mul(mul(mesh.transform,float4(vertex.position, 1.0f)), g_SceneGlobals.camera.viewProjection);
    result.normal = mul(vertex.normal, (float3x3) mesh.transformInvTranspose);
    result.tangent = mul(vertex.tangent, (float3x3) mesh.transformInvTranspose);
    result.uv = vertex.uv;
    return result;
}

float4 ps_main(PSInput input) : SV_TARGET
{
    SceneMeshInstance mesh = g_SceneMeshInstances[g_MeshIndex];
    SceneMaterial material = g_Materials[mesh.materialIndex];
    float4 albedo = material.albedo;
    if (material.albedoMap != INVALID_HEAP_HANDLE)
    {
        Texture2D albedoMap = ResourceDescriptorHeap[material.albedoMap];
        albedo = albedoMap.Sample(g_Sampler, input.uv);
    }
    return albedo;
}
