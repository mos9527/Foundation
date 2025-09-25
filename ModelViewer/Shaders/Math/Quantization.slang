module Quantization;
const static float EPS = 1e-6f;
// @ref Math::unpackUnitOctahedral
public const float3 unpackUnitOctahedral(float2 v)
{
    float3 nor = float3(v.xy, 1.0f - abs(v.x) - abs(v.y));
    nor.xy = nor.z >= EPS ? nor.xy :  (float2(1.0f, 1.0f) - abs(nor.yx)) * sign(nor.xy + EPS);
    return normalize(nor);
}
// @ref Math::dequantizeSnorm
public const float dequantizeSnorm(int32_t q, int32_t Nbits) {
    return q / (float)((1 << (Nbits - 1)) - 1);
}
// @ref Math::dequantizeSnormShifted
public const float dequantizeSnormShifted(int32_t q, int32_t Nbits) {
    return dequantizeSnorm(q - (1 << (Nbits - 1)), Nbits);
}
// Construct Normal, Tangent, Bitangent sign from packed representation
public void unpackTBN(
    uint32_t np /* 15-15-2 octa packed normal xyz + bitangent sign */,
    uint16_t tp /* 8-8 octa packed tagent xyz */,
    out float3 N /* normal */,
    out float4 T_BSign /* tangent, bitangent sign */
) {
    int3 inor = ((int3(np) >> int3(0, 15, 30)) & ((1u << 15u) - 1u)) - ((1u << 14u) - 1);
    int3 itan = ((int3(tp) >> int3(0, 8, 16)) & ((1u << 8u) - 1u)) - ((1u << 7u) - 1);
    N = unpackUnitOctahedral(float2(dequantizeSnorm(inor.x, 15), dequantizeSnorm(inor.y, 15)));
    T_BSign = float4(unpackUnitOctahedral(float2(dequantizeSnorm(itan.x, 8), dequantizeSnorm(itan.y, 8))), (inor.z < 0 ? -1.0f : 1.0f));
}
