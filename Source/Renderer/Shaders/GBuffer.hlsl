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
    float4 color : COLOR;
};

PSInput vs_main(VSInput vertex)
{
    PSInput result;

    result.position = float4(vertex.position, 1.0f);

    return result;
}

float4 ps_main(PSInput input) : SV_TARGET
{
    return input.color;
}
