#include "ICommon.slang"
[[SpecializationConstant]] int flags;
uniform UBO globalParams;
[[vk_binding(8,0)]] RWTexture2D<uint32_t> overdraw;
[[vk_binding(9,0)]] StructuredBuffer<GSMaterial> materials;
[[vk_binding(10,0)]] SamplerState textureSampler;
[[vk_binding(0,1)]] Texture2D<float4> textures[];

struct Fragment
{
    float4 RT0 : SV_Target0;
    float4 RT1 : SV_Target1;
    float4 RT2 : SV_Target2;
};

Fragment drawMeshlet(V2F input)
{
    Fragment output;
    uint32_t hashed = hash(input.meshlet);
    output.RT0 = float4(unpackUnorm4x8(hashed).xyz, 1.0);
    return output;
}

Fragment drawGBuffer(V2F input){
    Fragment output;
    GSMaterial material = materials[input.materialIndex + globalParams.firstMaterial];
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTexture)
        baseColor *= textures[material.baseColorTexture].Sample(textureSampler, input.uv);
    float4 normalTBN = float4(0,0,1,0);
    if (material.normalTexture)
        normalTBN = textures[material.normalTexture].Sample(textureSampler, input.uv) * 2 - float4(1);
    float4 pbrMetalRough = float4(1, material.metallicFactor, material.roughnessFactor, 1);
    if (material.metallicRoughnessTexture)
        pbrMetalRough *= textures[material.metallicRoughnessTexture].Sample(textureSampler, input.uv);
    float4 emissive = float4(material.emissiveFactor,0);
    if (material.emissiveTexture)
        emissive *= textures[material.emissiveTexture].Sample(textureSampler, input.uv);
    // -- GBuffer Packing
    float3 normal = normalize(input.tangent * normalTBN.x + input.bitangent * normalTBN.y + input.normal * normalTBN.z);
    float3 normal888 = packTBN888(normal, input.tangent);
    output.RT0 = float4(baseColor.xyz, input.materialIndex / 255.0f); // BaseColor, ID
    output.RT1 = float4(normal888, pbrMetalRough.y); // Normal 888, Metallic
    output.RT2 = float4(emissive.xyz, pbrMetalRough.x); // Emissive, Roughness
    return output;
}

import glsl; // https://github.com/shader-slang/slang/issues/4120
[shader("fragment")]
Fragment main(V2F input, float2 fragCoord : SV_Position /* pixel space */)
{
    Fragment output;
    if (flags & kViewMeshlet)
        output = drawMeshlet(input);
    else
        output = drawGBuffer(input);
    if (flags & kViewOverdraw)
        imageAtomicAdd(overdraw, int2(fragCoord), 1);
    return output;
}
