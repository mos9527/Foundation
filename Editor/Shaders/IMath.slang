// -- typedef
public typedef float3 SampledSpectrum; // RGB spectrum
// -- const
public static const float Pi = acos(-1);
public static const float InvPi = 1.0f / Pi;
public static const float Inv4Pi = 1.0f / (4.0f * Pi);
public static const float INF = 1e18f;
public static const float EPS = 1e-5f;
public static const float3 LUMINANCE_RGB = float3(0.2126f, 0.7152f, 0.0722f);
public bool AnyLuminance(float3 rgb) {
    return dot(rgb, LUMINANCE_RGB) > EPS;
}
// -- bits
public const uint32_t alignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
public struct complex
{
    public float2 c; // (real, imag)

    public __init(float re, float im = 0.0f) { c = float2(re, im); }
    public __init(float2 v) { c = v; }

    // Use properties for cleaner access
    public property real: float { get { return c.x; } }
    public property imag: float { get { return c.y; } }

    public complex conj() { return complex(c.x, -c.y); }
    public float norm() { return dot(c, c); }
    public float abs() { return length(c); }

    public complex mul(complex r) {
        return complex(
            c.x * r.c.x - c.y * r.c.y,
            c.x * r.c.y + c.y * r.c.x
        );
    }

    public complex sqrt() {
        float m = length(c);
        if (m <= 0.0f) return complex(0.0f, 0.0f);
        float term1 = sqrt(0.5f * (m + abs(c.x)));
        float term2 = 0.5f * c.y / term1;
        if (c.x >= 0.0f) {
            return complex(term1, term2);
        } else {
            return complex(abs(term2), (c.y >= 0.0f) ? term1 : -term1);
        }
    }
};

// Overloads
public complex operator+(complex l, complex r) { return complex(l.c + r.c); }
public complex operator-(complex l, complex r) { return complex(l.c - r.c); }
public complex operator*(complex l, complex r) { return l.mul(r); }
public complex operator/(complex l, complex r) {
    float denom = dot(r.c, r.c);
    complex num = l.mul(r.conj());
    return complex(num.c / denom);
}
public float real(complex c) { return c.real; }
public float imag(complex c) { return c.imag; }
public float abs(complex c)  { return c.abs(); }
public float norm(complex c) { return c.norm(); }
public complex sqrt(complex c) { return c.sqrt(); }
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
// Quaternion that rotates input vector to (0, 0, 1)
// Same as PBRT's Frame::ToZ, tangent frame is implicitly defined by this rotation
public quat quatRotateToZ(float3 input) {

	// Handle special case when input is exact or near opposite of (0, 0, 1)
	if (input.z < EPS - 1) return quat(float4(1.0f, 0.0f, 0.0f, 0.0f));
	return quat(normalize(float4(input.y, -input.x, 0.0f, 1.0f + input.z)));
}
// -- de/quantization
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
// Building an Orthonormal Basis from a 3D Unit Vector Without Normalization - Frisvad, 2012
public void buildOrthonormalBasis(const float3 n, out float3 b1,out float3 b2)
{
    if (n.z < EPS - 1)
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
    uint32_t nX = quantizeSnormShifted(oct.x, 12), nY = quantizeSnormShifted(oct.y, 12);
    uint32_t tA = quantizeSnormShifted(octAngle, 7);
    uint32_t bS = bitangentSign >= 0.0f ? 1 : 0;
    uint32_t tbn32 = 0;
    tbn32 = bitfieldInsert(tbn32, nX, 0, 12);
    tbn32 = bitfieldInsert(tbn32, nY, 12, 12);
    tbn32 = bitfieldInsert(tbn32, tA, 24, 7);
    tbn32 = bitfieldInsert(tbn32, bS, 31, 1);
    return tbn32;
}
public void unpackTBN(uint32_t packed, out float3 outNormal, out float3 outTangent, out float outBitangentSign) {
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
public uint hash(uint2 v)
{
    return hash(v.x ^ hash(v.y));
}
public uint hash(uint3 v)
{
    return hash(v.x ^ hash(v.y ^ hash(v.z)));
}
public uint hash(uint4 v)
{
    return hash(v.x ^ hash(v.y ^ hash(v.z ^ hash(v.w))));
}
// -- RNG
// In [0, 1) range
float uintToFloat(uint x) {
	return asfloat(0x3f800000 | (x >> 9)) - 1.0f;
}

public interface ISampler
{
    [mutating] float sample();
    [mutating] float2 sample2D();
}

// https://www.pbr-book.org/4ed/Sampling_Algorithms/Reservoir_Sampling
public interface IReservoirCandidate
{
    bool IsValid();
}

public struct WeightedReservoir<T : IReservoirCandidate>
{
    public T selected;
    public float selectedWeight;
    public float weightSum;
    public uint32_t sampleCount;

    public __init()
    {
        this.selected = T();
        this.selectedWeight = 0.0f;
        this.weightSum = 0.0f;
        this.sampleCount = 0u;
    }

    [mutating] public void Add(T candidate, float weight, float u)
    {
        if (!candidate.IsValid() || weight <= 0.0f || any(isnan(float3(weight, u, this.weightSum))))
            return;

        this.sampleCount++;
        this.weightSum += weight;
        if (u * this.weightSum < weight)
        {
            this.selected = candidate;
            this.selectedWeight = weight;
        }
    }

    public bool HasSample()
    {
        return this.sampleCount > 0u && this.weightSum > 0.0f && this.selected.IsValid();
    }

    public float SelectionPDF()
    {
        return this.weightSum > 0.0f ? this.selectedWeight / this.weightSum : 0.0f;
    }
}

// PCG random numbers generator
// Source: "Hash Functions for GPU Rendering" by Jarzynski & Olano
public struct PCG : ISampler
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
    public __init(float4 initState)
    {
        state = asuint(initState);
    }
    [mutating] public float sample()
    {
        state.w++;
        state = pcg4d(state);
        return uintToFloat(state.x);
    }
    [mutating] public float2 sample2D()
    {
        return float2(sample(), sample());
    }
}
// https://www.pbr-book.org/4ed/Sampling_and_Reconstruction/Sobol_Samplers#SobolSample
public struct Sobol : ISampler
{
    uint sampleIndex;
    uint dimension;
    uint seed;
    StructuredBuffer<uint> matrices;

    public __init(uint2 pixel, uint frameNo, StructuredBuffer<uint> sobolMatrices)
    {
        sampleIndex = frameNo;
        dimension = 0;
        seed = hash(pixel);
        matrices = sobolMatrices;
    }
    [mutating] public float sample()
    {
        uint v = 0, index = sampleIndex;
        // See Render/Tables.hpp
        for (uint i = dimension * 52; index != 0; index >>= 1, i++)
            if ((index & 1) != 0)
                v ^= matrices[i];
        dimension++;
        // Hash Scramble
        v ^= hash(uint2(seed, dimension));
        return float(v) * (1.0f / 4294967296.0f);
    }
    [mutating] public float2 sample2D()
    {
        return float2(sample(), sample());
    }
}

public static const float EPS_TMin = 1e-2;
// The safeOffsetRay function shifts a point on a surface so that a ray
// bouncing off the surface is no longer treated as intersecting the surface
// from which it originated when tMin = 0.0.
//
// This uses the technique by Carsten Wächter and Nikolaus Binder
// from "A Fast and Robust Method for Avoiding Self-Intersection" in Ray
// Tracing Gems (2019).
// The offset direction is typically the surface normal or its negation,
// depending on whether the ray exits or enters the surface.
public float3 OffsetRay(float3 worldPosition, float3 offsetDirection)
{
    // Convert the offset direction to an integer offset.
    const float scaleValue = 256.0f;
    const int3 scaleInt = int3(scaleValue * offsetDirection);

    // Offset each component of worldPosition using its binary representation.
    // Handle the sign bits correctly.
    const float3 offsetPosition = float3(
        asfloat(asint(worldPosition.x) + ((worldPosition.x < 0) ? -scaleInt.x : scaleInt.x)),
        asfloat(asint(worldPosition.y) + ((worldPosition.y < 0) ? -scaleInt.y : scaleInt.y)),
        asfloat(asint(worldPosition.z) + ((worldPosition.z < 0) ? -scaleInt.z : scaleInt.z)));

    // Use a floating-point offset instead for points near (0,0,0), the origin.
    const float origin = 1.0f / 32.0f;
    const float floatScale = 1.0f / 65536.0f;
    return float3(
        abs(worldPosition.x) < origin ? worldPosition.x + floatScale * offsetDirection.x : offsetPosition.x,
        abs(worldPosition.y) < origin ? worldPosition.y + floatScale * offsetDirection.y : offsetPosition.y,
        abs(worldPosition.z) < origin ? worldPosition.z + floatScale * offsetDirection.z : offsetPosition.z);
}