static const float PI = acos(-1);
// -- bits
public const uint32_t alignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
// -- quaternion
public struct quat
{
    float4 q; // (x, y, z, w)
    public __init(float4 v) { q = v; }
    public __init(float3 axis, float angle) {
        float halfAngle = angle * 0.5;
        float s = sin(halfAngle);
        q = float4(axis * s, cos(halfAngle));
    }
    // Rotate a vector by this quaternion
    public float3 rotate(float3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
    // Multiply two quaternions
    public quat mul(quat r) {
        return quat(float4(
            q.w * r.q.x + q.x * r.q.w + q.y * r.q.z - q.z * r.q.y,
            q.w * r.q.y - q.x * r.q.z + q.y * r.q.w + q.z * r.q.x,
            q.w * r.q.z + q.x * r.q.y - q.y * r.q.x + q.z * r.q.w,
            q.w * r.q.w - q.x * r.q.x - q.y * r.q.y - q.z * r.q.z
        ));
    }
    // Overload
    public quat operator*(quat r) { return mul(r); }
};
// -- dequantization
const static float EPS = 1e-6f;
float dequantizeSnorm(int32_t q, int32_t Nbits) {
    return q / (float)((1 << (Nbits - 1)) - 1);
}
float dequantizeSnormShifted(int32_t q, int32_t Nbits) {
    return dequantizeSnorm(q - (1 << (Nbits - 1)), Nbits);
}
// -- tangent frame
void buildOrthonormalBasis(const float3 n, out float3 b1,out float3 b2)
{
    if (n.z < -0.9999999)
    {
        b1 = float3(0.0, -1.0, 0.0);
        b2 = float3(-1.0, 0.0, 0.0);
    }
    float a = 1.0 / (1.0 + n.z);
    float b = -n.x * n.y * a;
    b1 = float3(1.0 - n.x * n.x * a, b, -n.x);
    b2 = float3(b, 1.0 - n.y * n.y * a, -n.y);
}
float3 unpackUnitOctahedralSnorm(float2 v)
{
    float3 nor = float3(v.xy, 1.0f - abs(v.x) - abs(v.y));
    nor.xy = nor.z >= EPS ? nor.xy :  (float2(1.0f, 1.0f) - abs(nor.yx)) * sign(nor.xy + EPS);
    return normalize(nor);
}
// R2, L2 to L1 projection on unit circle
float2 unpackUnitCircleSnorm(float v){
    float x = abs(v) * 2.0f - 1.0f;
    float y = 1.0f - abs(x);
    return v >= 0.0f ? float2(x, y) : float2(x, -y);
}
void unpackTBN(uint32_t packed, out float3 outNormal, out float3 outTangent, out float outBitangentSign) {
    uint32_t nX = bitfieldExtract(packed, 0, 12);
    uint32_t nY = bitfieldExtract(packed, 12, 12);
    uint32_t tA = bitfieldExtract(packed, 24, 7);
    uint32_t bS = bitfieldExtract(packed, 31, 1);
    float2 normalOct = float2(dequantizeSnormShifted(nX, 12), dequantizeSnormShifted(nY, 12));
    outNormal = unpackUnitOctahedralSnorm(normalOct);
    float octAngle = dequantizeSnormShifted(tA, 7);
    float2 octXY = unpackUnitCircleSnorm(octAngle);
    float3 b1, b2;
    buildOrthonormalBasis(outNormal, b1, b2);
    outTangent = octXY.x * b1 + octXY.y * b2;
    outBitangentSign = bS == 1 ? 1.0f : -1.0f;
}
