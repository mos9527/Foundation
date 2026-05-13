import IBindless2D;
import ICommon;
import IFlags;
import IMath;
[[SpecializationConstant]] int flags;
uniform UBO globalParams;
[[vk_binding(5,0)]] StructuredBuffer<GSMaterial> materials;
[[vk_binding(6,0)]] SamplerState textureSampler;
[[vk_binding(7,0)]] RWTexture2D<uint32_t> overdraw;

struct Fragment
{
    float4 RT0 : SV_Target0;
    float4 RT1 : SV_Target1;
    float4 RT2 : SV_Target2;
    uint32_t RT3 : SV_Target3; // Instance ID map for picking
};

Fragment drawMeshlet(V2F input)
{
    Fragment output;
    uint32_t hashed = hash(input.meshlet);
    output.RT0 = float4(unpackUnorm4x8(hashed).xyz, 1.0);
    output.RT1 = float4(0);
    output.RT2 = float4(0);
    output.RT3 = input.instanceID - globalParams.firstInstance;
    return output;
}

Fragment drawGBuffer(V2F input){
    Fragment output;
    GSMaterial material = materials[globalParams.firstMaterial + input.materialIndex];
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTexture != ~0u)
        baseColor *= textures[NonUniformResourceIndex(material.baseColorTexture)].Sample(textureSampler, input.uv);
    float4 normalTBN = float4(0,0,1,0);
    if (material.normalTexture != ~0u)
        normalTBN = textures[NonUniformResourceIndex(material.normalTexture)].Sample(textureSampler, input.uv) * 2 - float4(1);
    float4 pbrMetalRough = float4(1, material.roughnessFactor, material.metallicFactor, 1);
    if (material.metallicRoughnessTexture != ~0u)
        pbrMetalRough *= textures[NonUniformResourceIndex(material.metallicRoughnessTexture)].Sample(textureSampler, input.uv);
    float4 emissive = float4(material.emissiveFactor,0);
    if (material.emissiveTexture != ~0u)
        emissive *= textures[NonUniformResourceIndex(material.emissiveTexture)].Sample(textureSampler, input.uv);
    // -- GBuffer Packing
    float3 normal = normalize(input.tangent * normalTBN.x + input.bitangent * normalTBN.y + input.normal * normalTBN.z);
    float3 normal888 = packTBN888(normal, input.tangent);
    uint ID = 0; // Material ID [7] Handness [1]
    ID = bitfieldInsert(ID, input.materialIndex, 0, 7);
    ID = bitfieldInsert(ID, input.bitangentSign >= 0.0f ? 1 : 0, 7, 1);
    output.RT0 = float4(baseColor.xyz, (float)ID / 255.0f); // BaseColor, ID
    output.RT1 = float4(normal888, pbrMetalRough.z); // Normal 888, Metallic
    output.RT2 = float4(emissive.xyz, pbrMetalRough.y); // Emissive, Roughness
    output.RT3 = input.instanceID - globalParams.firstInstance;
    return output;
}

import glsl; // https://github.com/shader-slang/slang/issues/4120
[shader("fragment")]
Fragment main(V2F input, float2 fragCoord : SV_Position /* pixel space */)
{
    Fragment output;
    if ((flags & kViewMeshlet) != 0)
        output = drawMeshlet(input);
    else
        output = drawGBuffer(input);
    if ((flags & kViewOverdraw) != 0)
        imageAtomicAdd(overdraw, int2(fragCoord), 1);
    return output;
}