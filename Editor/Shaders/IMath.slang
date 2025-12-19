public static const float Pi = acos(-1);
public static const float InvPi = 1.0f / Pi;
public static const float INF = 1e18f;
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
    public quat inverted() {
        return quat(float4(-q.xyz, q.w));
    }
    // Overload
    public quat operator*(quat r) { return mul(r); }
};
// Quaternion that rotates input vector to +Z=(0, 0, 1)
public quat quatRotateToZ(float3 input) {

	// Handle special case when input is exact or near opposite of (0, 0, 1)
	if (input.z < EPS - 1.0f) return quat(float4(1.0f, 0.0f, 0.0f, 0.0f));
	return quat(normalize(float4(input.y, -input.x, 0.0f, 1.0f + input.z)));
}
// -- de/quantization
public static const float EPS = 1e-5f;
public uint32_t quantizeUnorm(float v, int32_t N)
{
    float scale = (float)((1 << N) - 1);

    v = (v >= 0) ? v : 0;
    v = (v <= 1) ? v : 1;

    return (uint32_t)(v * scale + 0.5f);
}
public float dequantizeUnorm(int32_t q, int32_t Nbits) {
    return q / (float)((1 << Nbits) - 1);
}
public int32_t quantizeSnorm(float v, int32_t N)
{
    float scale = (float)((1 << (N - 1)) - 1);
    float round = (v >= 0 ? 0.5f : -0.5f);

    v = (v >= -1) ? v : -1;
    v = (v <= +1) ? v : +1;

    return (int32_t)(v * scale + round);
}
public float dequantizeSnorm(int32_t q, int32_t Nbits) {
    return q / (float)((1 << (Nbits - 1)) - 1);
}
public uint32_t quantizeSnormShifted(float v, int32_t Nbits)
{
    return quantizeSnorm(v, Nbits) + (1 << (Nbits - 1));
}
public float dequantizeSnormShifted(int32_t q, int32_t Nbits) {
    return dequantizeSnorm(q - (1 << (Nbits - 1)), Nbits);
}
// -- unit L1~L2 projection
public float2 packUnitOctahedralSnorm(float3 v)
{
    v /= float3(abs(v.x) + abs(v.y) + abs(v.z));
    return v.z >= EPS ? v.xy : (float2(1.0f) - abs(float2(v.yx))) * sign(float2(v.xy + EPS));
}
public float3 unpackUnitOctahedralSnorm(float2 v)
{
    float3 nor = float3(v.xy, 1.0f - abs(v.x) - abs(v.y));
    float2 xy = nor.z >= EPS ? v.xy : (float2(1.0f) - abs(float2(v.yx))) * sign(float2(v.xy + EPS));
    return normalize(float3(xy.x, xy.y, nor.z));
}
public float packUnitCircleSnorm(float2 v){
    v /= abs(v.x) + abs(v.y);
    return v.y >= EPS ? (v.x + 1.0f) * 0.5f : -(v.x + 1.0f) * 0.5f;
}
public float2 unpackUnitCircleSnorm(float v){
    float x = abs(v) * 2.0f - 1.0f;
    float y = 1.0f - abs(x);
    return normalize(v >= 0.0f ? float2(x, y) : float2(x, -y));
}
// -- tangent frame projection
public void buildOrthonormalBasis(const float3 n, out float3 b1,out float3 b2)
{
    if (n.z < -1 + EPS)
    {
        b1 = float3(0.0, -1.0, 0.0);
        b2 = float3(-1.0, 0.0, 0.0);
        return;
    }
    float a = 1.0 / (1.0 + n.z);
    float b = -n.x * n.y * a;
    b1 = float3(1.0 - n.x * n.x * a, b, -n.x);
    b2 = float3(b, 1.0 - n.y * n.y * a, -n.y);
}
public uint32_t packTBN(in float3 normal, in float3 tangent, in float bitangentSign)
{
    float3 b1, b2;
    buildOrthonormalBasis(normal, b1, b2);
    float cosAngle = dot(tangent, b1), sinAngle = dot(tangent, b2);
    float octAngle = packUnitCircleSnorm(float2(cosAngle, sinAngle));
    float2 oct = packUnitOctahedralSnorm(normal);
    uint32_t nX = quantizeSnormShifted(oct.x, 10), nY = quantizeSnormShifted(oct.y, 10);
    uint32_t tA = quantizeSnormShifted(octAngle, 10);
    uint32_t bS = bitangentSign >= 0.0f ? 1 : 0;
    uint32_t tbn32 = 0;
    tbn32 = bitfieldInsert(tbn32, nX, 0, 10);
    tbn32 = bitfieldInsert(tbn32, nY, 10, 10);
    tbn32 = bitfieldInsert(tbn32, tA, 20, 10);
    tbn32 = bitfieldInsert(tbn32, bS, 30, 2);
    return tbn32;
}
public void unpackTBN(uint32_t packed, out float3 outNormal, out float3 outTangent, out float outBitangentSign) {
    uint32_t nX = bitfieldExtract(packed, 0, 10);
    uint32_t nY = bitfieldExtract(packed, 10, 10);
    uint32_t tA = bitfieldExtract(packed, 20, 10);
    uint32_t bS = bitfieldExtract(packed, 30, 2);
    float2 normalOct = float2(dequantizeSnormShifted(nX, 10), dequantizeSnormShifted(nY, 10));
    outNormal = unpackUnitOctahedralSnorm(normalOct);
    float octAngle = dequantizeSnormShifted(tA, 10);
    float2 octXY = unpackUnitCircleSnorm(octAngle);
    float3 b1, b2;
    buildOrthonormalBasis(outNormal, b1, b2);
    outTangent = octXY.x * b1 + octXY.y * b2;
    outBitangentSign = bS == 1 ? 1.0f : -1.0f;
}
// 3 byte tangent frame [UNORM xyz]
public float3 packTBN888(in float3 normal, in float3 tangent)
{
    float3 b1, b2;
    buildOrthonormalBasis(normal, b1, b2);
    float cosAngle = dot(tangent, b1), sinAngle = dot(tangent, b2);
    float2 oct = packUnitOctahedralSnorm(normal);
    float octAngle = packUnitCircleSnorm(float2(cosAngle, sinAngle));
    return float3(oct * 0.5f + 0.5f, octAngle * 0.5f + 0.5f);
}
public void unpackTBN888(in float3 packed, out float3 outNormal, out float3 outTangent) {
    float2 normalOct = packed.xy * 2.0f - 1.0f;
    outNormal = unpackUnitOctahedralSnorm(normalOct);
    float octAngle = packed.z * 2.0f - 1.0f;
    float2 octXY = unpackUnitCircleSnorm(octAngle);
    float3 b1, b2;
    buildOrthonormalBasis(outNormal, b1, b2);
    outTangent = octXY.x * b1 + octXY.y * b2;
}
// -- hashing
// https://burtleburtle.net/bob/hash/integer.html
public uint hash(uint a)
{
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
}
// -- RNG
// In [0, 1) range
float uintToFloat(uint x) {
	return asfloat(0x3f800000 | (x >> 9)) - 1.0f;
}
// PCG random numbers generator
// Source: "Hash Functions for GPU Rendering" by Jarzynski & Olano
public struct PCG
{
    uint4 pcg4d(uint4 v)
    {
    	v = v * 1664525u + 1013904223u;

    	v.x += v.y * v.w;
    	v.y += v.z * v.x;
    	v.z += v.x * v.y;
    	v.w += v.y * v.z;

    	v = v ^ (v >> 16u);

    	v.x += v.y * v.w;
    	v.y += v.z * v.x;
    	v.z += v.x * v.y;
    	v.w += v.y * v.z;

    	return v;
    }
    uint4 state;
    public __init(uint2 pixel, uint frameNo, uint seed = 0)
    {
        state = uint4(pixel, frameNo, seed);
    }
    [mutating] public float sample()
    {
        state.w++;
        state = pcg4d(state);
        return uintToFloat(state.x);
    }
    public float2 sample2()
    {
        return float2(sample(), sample());
    }
}