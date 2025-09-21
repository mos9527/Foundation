float2 PackUnitOctahedral(float3 v) {
    v /= float3(abs(v.x) + abs(v.y) + abs(v.z));
    v.xy = (v.z >= 0.0f) ? v.xy : (float2(1.0f) - abs(float2(v.yx))) * sign(float2(v.xy));
    return v.xy;
}

float3 UnpackUnitOctahedral(float2 v)
{
    float3 nor = float3(v.xy, 1.0f - abs(v.x) - abs(v.y));
    nor.xy = nor.z >= 0.0f ? nor.xy :  (float2(1.0f, 1.0f) - abs(nor.yx)) * sign(nor.xy);
    return normalize(nor);
}


float DequantizeSnorm(int32_t q, int32_t Nbits) {
    return q / (float)((1 << (Nbits - 1)) - 1);
}

void UnpackTBN(uint32_t np /* 15-15-2 octa xy + bitangent sign */, uint16_t tp /* 8-8 octa xy */, out float3 N, out float4 T_BSign) {
    int3 inor = ((int3(np) >> int3(0, 15, 30)) & ((1u << 15u) - 1u)) - ((1u << 14u) - 1);
    int3 itan = ((int3(tp) >> int3(0, 8, 16)) & ((1u << 8u) - 1u)) - ((1u << 7u) - 1);
    N = UnpackUnitOctahedral(float2(DequantizeSnorm(inor.x, 15), DequantizeSnorm(inor.y, 15)));
    T_BSign = float4(UnpackUnitOctahedral(float2(DequantizeSnorm(itan.x, 8), DequantizeSnorm(itan.y, 8))), (inor.z < 0 ? -1.0f : 1.0f));    
}
float3 Translate(float3 v, float3 t)
{
    return v + t;
}
float4 qmul(float4 q, float4 r)
{
    float3 qv = q.xyz, rv = r.xyz;
    return float4(q.w * rv + r.w * qv + cross(qv, rv), q.w * r.w - dot(qv, rv));
}
float3 RotateSlow(float3 v, float4 q)
{
    float4 p = float4(v, 0.0f);
    float4 qConj = float4(-q.xyz, q.w);
    float4 rotatedP = qmul(qmul(q, p), qConj);
    return rotatedP.xyz;
}
float3 Rotate(float3 v, float4 q)
{
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}