import ICommon;
import IFlags;
import IMath;
[[SpecializationConstant]] int flags;
uniform UBO globalParams;
// Bindings 3..5: shared by the meshlet path (EMSBasic, bindings 1,2,6,7) and the indirect-draw
// path (EVSIndirectDraw, bindings 1,2) so this fragment is reused unmodified by both. Each set's
// bindings must be contiguous, hence the gbuffer slots sit directly after the common 0..2.
[[vk_binding(3,0)]] StructuredBuffer<GSMaterial> materials;
[[vk_binding(4,0)]] SamplerState textureSampler;
[[vk_binding(5,0)]] RWTexture2D<uint32_t> overdraw;
[[vk_binding(0,1)]] Texture2D<float4> textures[];
struct Fragment
{
    float4 RT0 : SV_Target0;
    float4 RT1 : SV_Target1;
    float3 RT2 : SV_Target2;
    uint32_t RT3 : SV_Target3; // Instance ID map for picking
};

Fragment drawMeshlet(V2F input)
{
    Fragment output;
    uint32_t hashed = hash(input.meshlet);
    output.RT0 = float4(unpackUnorm4x8(hashed).xyz, 1.0);
    output.RT1 = float4(0);
    output.RT2 = float3(0);
    output.RT3 = input.instanceID - globalParams.instances.offset;
    return output;
}

Fragment drawTextureLOD(V2F input)
{
    Fragment output;
    GSMaterial material = materials[globalParams.materials.offset + input.materialIndex];
    float3 color = float3(0.0f);
    if (material.baseColorTexture != ~0u)
    {
        uint width, height;
        textures[NonUniformResourceIndex(material.baseColorTexture)].GetDimensions(width, height);
        float mipLevel = (flags & kForceTextureLOD0) != 0
            ? 0.0f
            : ApproximateMipLevelFromUVGradient(ddx(input.uv), ddy(input.uv), width, height);
        color = VisualizeApproximateMipLevel(mipLevel);
    }
    output.RT0 = float4(color, 1.0f);
    output.RT1 = float4(0);
    output.RT2 = float3(0);
    output.RT3 = input.instanceID - globalParams.instances.offset;
    return output;
}

float4 SampleMaterialTexture(uint textureIndex, float2 uv)
{
    if ((flags & kForceTextureLOD0) != 0)
        return textures[NonUniformResourceIndex(textureIndex)].SampleLevel(textureSampler, uv, 0);
    return textures[NonUniformResourceIndex(textureIndex)].Sample(textureSampler, uv);
}

Fragment drawGBuffer(V2F input){
    Fragment output;
    int materialFlags = globalParams.dbgMaterialFlags;
    GSMaterial material = materials[globalParams.materials.offset + input.materialIndex];
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTexture != ~0u)
        baseColor *= SampleMaterialTexture(material.baseColorTexture, input.uv);
    if ((materialFlags & kMaterialDbgWhiteBaseColor) != 0u)
        baseColor = float4(1.0f, 1.0f, 1.0f, baseColor.a);
    // TODO TODO TODO Actual transparency pipeline    
    if (baseColor.a < 0.5f)
        discard;
    float4 normalTBN = float4(0,0,1,0);
    if (material.normalTexture != ~0u)
    {        
        float2 xy = (SampleMaterialTexture(material.normalTexture, input.uv).xy * 2 - 1) * material.normalScale;
        normalTBN.xyz = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    }
    float4 pbrMetalRough = float4(1, material.roughnessFactor, material.metallicFactor, 1);
    if (material.metallicRoughnessTexture != ~0u)
        pbrMetalRough *= SampleMaterialTexture(material.metallicRoughnessTexture, input.uv);
    float4 emissive = float4(material.emissiveFactor,0);
    if (material.emissiveTexture != ~0u)
        emissive *= SampleMaterialTexture(material.emissiveTexture, input.uv);
    // -- GBuffer Packing
    float3 normal = normalize(input.tangent * normalTBN.x + input.bitangent * normalTBN.y + input.normal * normalTBN.z);
    float2 normalOct = packUnitOctahedralSnorm(normal) * 0.5f + 0.5f;
    output.RT0 = float4(baseColor.xyz, pbrMetalRough.y); // BaseColor, Roughness
    output.RT1 = float4(normalOct, pbrMetalRough.z, 0.0f);
    output.RT2 = float3(emissive.xyz); // Emissive
    output.RT3 = input.instanceID - globalParams.instances.offset;
    return output;
}

import glsl; // https://github.com/shader-slang/slang/issues/4120
[shader("fragment")]
Fragment main(V2F input, float2 fragCoord : SV_Position /* pixel space */)
{
    Fragment output;
    if ((flags & kViewMeshlet) != 0)
        output = drawMeshlet(input);
    else if ((flags & kViewTextureLOD) != 0)
        output = drawTextureLOD(input);
    else
        output = drawGBuffer(input);
    if ((flags & kViewOverdraw) != 0)
        imageAtomicAdd(overdraw, int2(fragCoord), 1);
    return output;
}