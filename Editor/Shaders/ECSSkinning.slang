import ICommon;

ByteAddressBuffer skinningInput;
ByteAddressBuffer skinningPalette;
RWByteAddressBuffer dynamicPrimitives;

struct PushConstant
{
    uint32_t bindVertexOffset;
    uint32_t skinBindingOffset;
    uint32_t vertexDestinationOffset;
    uint32_t paletteOffset;
    uint32_t vertexCount;
    uint32_t jointCount;
};
[[vk::push_constant]] PushConstant pc;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint32_t id : SV_DispatchThreadID)
{
    if (id >= pc.vertexCount)
        return;

    FQVertex source = skinningInput.Load<FQVertex>(pc.bindVertexOffset + id * sizeof(FQVertex));
    uint32_t bindingOffset = pc.skinBindingOffset + id * 24u;
    uint32_t joints01 = skinningInput.Load<uint32_t>(bindingOffset);
    uint32_t joints23 = skinningInput.Load<uint32_t>(bindingOffset + 4u);
    uint4 joints = uint4(joints01 & 0xffffu, joints01 >> 16u, joints23 & 0xffffu, joints23 >> 16u);
    float4 weights = float4(
        skinningInput.Load<float>(bindingOffset + 8u),
        skinningInput.Load<float>(bindingOffset + 12u),
        skinningInput.Load<float>(bindingOffset + 16u),
        skinningInput.Load<float>(bindingOffset + 20u));
    float weightSum = weights.x + weights.y + weights.z + weights.w;
    if (weightSum <= 0.0)
    {
        dynamicPrimitives.Store<FQVertex>(pc.vertexDestinationOffset + id * sizeof(FQVertex), source);
        return;
    }
    weights /= weightSum;

    float4x4 skin = float4x4(0.0);
    for (uint32_t i = 0; i < 4; ++i)
        if (weights[i] != 0.0 && joints[i] < pc.jointCount)
            skin += weights[i] * skinningPalette.Load<float4x4>(pc.paletteOffset + joints[i] * 64u);

    float3 position, normal, tangent;
    float bitangentSign;
    float2 uv;
    FQDecode(source, position, normal, tangent, bitangentSign, uv);
    position = mul(skin, float4(position, 1.0)).xyz;
    float3x3 basis = (float3x3)skin;
    normal = normalize(mul(basis, normal));
    tangent = normalize(mul(basis, tangent));
    dynamicPrimitives.Store<FQVertex>(
        pc.vertexDestinationOffset + id * sizeof(FQVertex),
        FQPack(position, normal, tangent, bitangentSign, uv));
}
