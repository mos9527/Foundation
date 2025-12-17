// References
// - RAY TRACING GEMS II - Chapter 14
// - https://github.com/boksajak/referencePT/blob/master/shaders/PathTracer.hlsl
#include "ICommon.slang"
#include "IBRDF.slang"
uniform UBO globalParams;
// -- Math
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
    public __init(uint2 pixel, uint seed = 0)
    {
        state = uint4(pixel, globalParams.frameNumber, seed);
    }
    [mutating] public float sample()
    {
        state.w++;
        state = pcg4d(state);
        return uintToFloat(state.x);
    }
}

// -- Scene
RaytracingAccelerationStructure AS;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitives;
StructuredBuffer<GSMaterial> materials;
SamplerState textureSampler;
RWTexture2D<float4> accumulation;
[[vk_binding(0,1)]] Texture2D<float4> textures[];
[[vk_binding(0,2)]] RWTexture2D<float4> output;

void IntersectTriangle(uint meshOffset, uint primitive, float2 bary, out float3 position, out float3 normal, out float3 tangent, out float2 uv, out float bitangentSign){
    GSMesh mesh = primitives.Load<GSMesh>(meshOffset);
    uint i0 = primitives.Load<uint32_t>(mesh.idxOffset + (primitive * 3 + 0) * sizeof(uint32_t));
    uint i1 = primitives.Load<uint32_t>(mesh.idxOffset + (primitive * 3 + 1) * sizeof(uint32_t));
    uint i2 = primitives.Load<uint32_t>(mesh.idxOffset + (primitive * 3 + 2) * sizeof(uint32_t));
    FQVertex vq0 = primitives.Load<FQVertex>(mesh.vtxOffset + i0 * sizeof(FQVertex));
    FQVertex vq1 = primitives.Load<FQVertex>(mesh.vtxOffset + i1 * sizeof(FQVertex));
    FQVertex vq2 = primitives.Load<FQVertex>(mesh.vtxOffset + i2 * sizeof(FQVertex));
    float3 p0, p1, p2;
    float3 n0, n1, n2;
    float3 t0, t1, t2;
    float bi0, bi1, bi2;
    float2 u0, u1, u2;
    FQDecode(vq0, p0, n0, t0, bi0, u0);
    FQDecode(vq1, p1, n1, t1, bi1, u1);
    FQDecode(vq2, p2, n2, t2, bi2, u2);
    position = p0 * (1 - bary.x - bary.y) + p1 * bary.x + p2 * bary.y;
    normal = normalize(n0 * (1 - bary.x - bary.y) + n1 * bary.x + n2 * bary.y);
    tangent = normalize(t0 * (1 - bary.x - bary.y) + t1 * bary.x + t2 * bary.y);
    bitangentSign = bi0 * (1 - bary.x - bary.y) + bi1 * bary.x + bi2 * bary.y;
    uv = u0 * (1 - bary.x - bary.y) + u1 * bary.x + u2 * bary.y;
}

void SampleMaterials(GSMaterial material, float2 uv,
    out float4 baseColor, out float4 normalTBN, out float4 pbrMetalRough, out float4 emissive)
{
    baseColor = material.baseColorFactor;
    if (material.baseColorTexture != 0)
        baseColor *= textures[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(textureSampler, uv, 0);
    normalTBN = float4(0,0,1,0);
    if (material.normalTexture != 0)
        normalTBN = textures[NonUniformResourceIndex(material.normalTexture)].SampleLevel(textureSampler, uv, 0) * 2 - float4(1);
    pbrMetalRough = float4(1, material.metallicFactor, material.roughnessFactor, 1);
    if (material.metallicRoughnessTexture != 0)
        pbrMetalRough *= textures[NonUniformResourceIndex(material.metallicRoughnessTexture)].SampleLevel(textureSampler, uv, 0);
    emissive = float4(material.emissiveFactor,0);
    if (material.emissiveTexture != 0)
        emissive *= textures[NonUniformResourceIndex(material.emissiveTexture)].SampleLevel(textureSampler, uv, 0);
}

float3 EvalSkyDome(float3 dir)
{
    return float3(1); // Furnace
}
// Specular over specular+diffuse
float EvalSpecularBRDFProbability(float3 baseColor, float metallic, float3 v, float3 n)
{
    float VoN = saturate(dot(v, n)); // Half-vector not known
    float Fspec = dot(LUMINANCE_RGB, F_Schlick(VoN, float3(0.04f))); // Dielectric F0, see also ECSLighting.slang
    float Fdiff = dot(LUMINANCE_RGB, baseColor * (1.0f - metallic)) * (1 - Fspec);
    return clamp(Fspec / (Fspec + Fdiff), 0.1f, 0.9f);
}
// Quaternion that rotates input vector to (0, 0, 1)
quat QuatRotateToZ(float3 input) {

	// Handle special case when input is exact or near opposite of (0, 0, 1)
	if (input.z < EPS - 1.0f) return quat(float4(1.0f, 0.0f, 0.0f, 0.0f));
	return quat(normalize(float4(input.y, -input.x, 0.0f, 1.0f + input.z)));
}

// -- Main Shader Routines
static const uint32_t INSTANCE_NO_HIT = ~0u;
struct HitPayload
{
    // Model space
    float3 position;
    float3 normal;
    float4 tangent;
    float2 uv;
    uint32_t instance;
    uint32_t material;

    bool IsHit() { return instance != INSTANCE_NO_HIT; }
};

[shader("raygeneration")]
void RayGeneration()
{
    // Generate primary ray with jitter for AA
    uint2 pix = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    PCG rng = PCG(pix);
    float2 jitter = lerp(float2(-0.5f), float2(0.5f), float2(rng.sample(), rng.sample()));
    float2 uv = float2(pix + jitter + 0.5f) / dim;
    float4 ndcPosition = float4(uv, 1.0f, 1.0f);
    ndcPosition.y = 1 - ndcPosition.y;
    ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
    float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);
    float3 p = wsPosition.xyz / wsPosition.w;
    float3 eye = globalParams.camPosition;
    float3 dir = normalize(p - eye);
    // Trace
    float3 radiance = float3(0);
    float3 throughput = float3(1);
    // Multiple bounces
    RayDesc ray;
    ray.Origin = p;
    ray.Direction = dir;
    ray.TMin = 1e-2;
    ray.TMax = 1e9;
    HitPayload payload;
    for (uint bounce = 0; bounce < globalParams.ptMaxBounces; bounce++){
        TraceRay(AS, 0, 0xFF, 0, 0, 0, ray, payload);
        if (!payload.IsHit()){
            // Miss - environment
            radiance += throughput * EvalSkyDome(ray.Direction);
            break;
        }
        // Shading
        GSInstance inst = instances[NonUniformResourceIndex(payload.instance)];
        GSMaterial material = materials[NonUniformResourceIndex(inst.materialIndex)];
        float4 baseColor, normalTBN, pbrMetalRough, emissive;
        SampleMaterials(material, payload.uv, baseColor, normalTBN, pbrMetalRough, emissive);
        float metallic = pbrMetalRough.z;
        float roughness = pbrMetalRough.y;
        float3 n = payload.normal, t = payload.tangent.xyz;
        n = inst.rotation.rotate(n);
        t = inst.rotation.rotate(t);
        float3 b = cross(n, t) * (payload.tangent.w > 0.0f ? 1 : -1);
        float3 geometryNormal = n;
        n = normalize(t * normalTBN.x + b * normalTBN.y + n * normalTBN.z);
        // Emissive
        radiance += throughput * emissive.xyz;
        // Local space
        quat toLocal = QuatRotateToZ(n);
        quat toWorld = toLocal.inverted();
        // Importance sample next ray
        // Choose diffuse/specular based on fresnel
        float3 v = -ray.Direction;
        float3 vLocal = toLocal.rotate(v);
        float3 nLocal = float3(0,0,1);
        float2 u = float2(rng.sample(), rng.sample());
        float3 rayLocal;
        float Pspec = EvalSpecularBRDFProbability(baseColor.xyz, metallic, v, n);
        if (false && rng.sample() < Pspec){ // TODO
            // Specular
        } else {
            // Diffuse
            // https://pbr-book.org/4ed/Reflection_Models/Diffuse_Reflection
            rayLocal = SampleCosineHemisphere(u);
            throughput *= baseColor.xyz * CosineHemispherePDF(saturate(dot(rayLocal, nLocal)));
        }
        ray.Direction = normalize(toWorld.rotate(rayLocal));
        // TODO Ray Offset
        ray.Origin = payload.position + geometryNormal * 1e-2;
    }
    // Transform world space point
    // Accumulation
    float4 previous = accumulation[pix];
    if (globalParams.ptAccumualatedFrames == 0)
        previous = float4(0);
    float4 current = previous + float4(radiance, 0);
    accumulation[pix] = current;
    float4 average = current / float(globalParams.ptAccumualatedFrames + 1);
    output[pix] = float4(average.xyz, 1.0f);
}

[shader("closesthit")]
void RayClosestHit(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceIndex();
    GSInstance inst = instances[NonUniformResourceIndex(instance)];
    GSMaterial material = materials[NonUniformResourceIndex(inst.materialIndex)];
    IntersectTriangle(inst.meshOffset, PrimitiveIndex(), attr.barycentrics.xy, payload.position, payload.normal, payload.tangent.xyz, payload.uv, payload.tangent.w);
    payload.instance = instance;
    payload.material = inst.materialIndex;
}

[shader("miss")]
void RayMiss(inout HitPayload payload)
{
    payload.instance = INSTANCE_NO_HIT;
}
