// References
// - PBRT v4
// - RAY TRACING GEMS II - Chapter 14
// - https://github.com/boksajak/referencePT/blob/master/shaders/PathTracer.hlsl
// - https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang
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
RWTexture2D<float4> output;
[[vk_binding(0,1)]] Texture2D<float4> textures[];


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
    pbrMetalRough = float4(1, material.roughnessFactor, material.metallicFactor, 1);
    if (material.metallicRoughnessTexture != 0)
        pbrMetalRough *= textures[NonUniformResourceIndex(material.metallicRoughnessTexture)].SampleLevel(textureSampler, uv, 0);
    emissive = float4(material.emissiveFactor,0);
    if (material.emissiveTexture != 0)
        emissive *= textures[NonUniformResourceIndex(material.emissiveTexture)].SampleLevel(textureSampler, uv, 0);
}
void DecodePayload(HitPayload payload,
    out float3 position, out float3 n, out float3 t, out float3 b, out float3 geometricNormal,
    out uint materialID, out float3 baseColor, out float3 emissive, out float metallic, out float roughness)
{
    GSInstance inst = instances[NonUniformResourceIndex(payload.instance)];
    materialID = inst.materialIndex;
    GSMaterial material = materials[NonUniformResourceIndex(materialID)];
    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    SampleMaterials(material, payload.uv, baseColor4, normalTBN, pbrMetalRough, emissive4);
    baseColor = baseColor4.xyz;
    emissive = emissive4.xyz;
    roughness = pbrMetalRough.y;
    metallic = pbrMetalRough.z;
    n = normalize(payload.normal);
    t = normalize(payload.tangent.xyz);
    n = inst.rotation.rotate(n);
    t = inst.rotation.rotate(t);
    position = inst.rotation.rotate(payload.position);
    position = position * inst.scale + inst.transform;
    b = cross(n, t) * (payload.tangent.w > 0.0f ? 1 : -1);
    geometricNormal = n;
    n = normalize(t * normalTBN.x + b * normalTBN.y + n * normalTBN.z);
}
float3 EvalSkyDome(float3 dir)
{
    return float3(0.1735); // Furnace
}
// Quaternion that rotates input vector to (0, 0, 1)
quat QuatRotateToZ(float3 input) {

	// Handle special case when input is exact or near opposite of (0, 0, 1)
	if (input.z < EPS - 1.0f) return quat(float4(1.0f, 0.0f, 0.0f, 0.0f));
	return quat(normalize(float4(input.y, -input.x, 0.0f, 1.0f + input.z)));
}
// The safeOffsetRay function shifts a point on a surface so that a ray
// bouncing off the surface is no longer treated as intersecting the surface
// from which it originated when tMin = 0.0.
//
// This uses the technique by Carsten Wächter and Nikolaus Binder
// from "A Fast and Robust Method for Avoiding Self-Intersection" in Ray
// Tracing Gems (2019).
// The offset direction is typically the surface normal or its negation,
// depending on whether the ray exits or enters the surface.
float3 OffsetRay(float3 worldPosition, float3 offsetDirection)
{
  // Convert the offset direction to an integer offset.
  const float scaleValue = 256.0f;
  const int3  scaleInt   = int3(scaleValue * offsetDirection);

  // Offset each component of worldPosition using its binary representation.
  // Handle the sign bits correctly.
  const float3 offsetPosition = float3(  //
      asfloat(asint(worldPosition.x) + ((worldPosition.x < 0) ? -scaleInt.x : scaleInt.x)),
      asfloat(asint(worldPosition.y) + ((worldPosition.y < 0) ? -scaleInt.y : scaleInt.y)),
      asfloat(asint(worldPosition.z) + ((worldPosition.z < 0) ? -scaleInt.z : scaleInt.z)));

  // Use a floating-point offset instead for points near (0,0,0), the origin.
  const float origin     = 1.0f / 32.0f;
  const float floatScale = 1.0f / 65536.0f;
  return float3(  //
      abs(worldPosition.x) < origin ? worldPosition.x + floatScale * offsetDirection.x : offsetPosition.x,
      abs(worldPosition.y) < origin ? worldPosition.y + floatScale * offsetDirection.y : offsetPosition.y,
      abs(worldPosition.z) < origin ? worldPosition.z + floatScale * offsetDirection.z : offsetPosition.z);
}

float3 GeneratePrimaryRay(uint2 pixel, PCG rng)
{
    float2 jitter = lerp(float2(-0.5f), float2(0.5f), float2(rng.sample(), rng.sample()));
    float2 uv = float2(pixel + jitter + 0.5f) / DispatchRaysDimensions().xy;
    float4 ndcPosition = float4(uv, 1.0f, 1.0f);
    ndcPosition.y = 1 - ndcPosition.y;
    ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
    float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);
    return normalize(wsPosition.xyz / wsPosition.w - globalParams.camPosition);
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
[[SpecializationConstant]] int flags;
[shader("raygeneration")]
void RayGeneration()
{
    uint2 pix = DispatchRaysIndex().xy;
    PCG rng = PCG(pix);
    // Generate primary ray with jitter for AA
    // Trace
    float3 radiance = float3(0);
    float3 throughput = float3(1);
    // Multiple bounces
    RayDesc ray;
    ray.Origin = globalParams.camPosition;
    ray.Direction = GeneratePrimaryRay(pix, rng);
    ray.TMin = 0.0f;
    ray.TMax = 1e9;
    HitPayload payload;
    for (uint bounce = 0; bounce < globalParams.ptMaxBounces; bounce++){
        TraceRay(AS, 0, 0xFF, 0, 0, 0, ray, payload);
        if (!payload.IsHit()){
            // Miss - environment
            radiance += throughput * EvalSkyDome(ray.Direction);
            break;
        }
        // Decode
        float3 p, n, t, b, geometricNormal;
        float3 baseColor, emissive;
        float metallic, roughness;
        uint materialID;
        DecodePayload(payload, p, n, t, b, geometricNormal, materialID, baseColor, emissive, metallic, roughness);
        // -- vv Debug View
        if (flags & (kViewBaseColor | kViewNormal | kViewMaterialID | kViewPosition)) {
            if (flags & kViewBaseColor) {
                radiance = baseColor;
            } else if (flags & kViewNormal) {
                radiance = n * 0.5f + 0.5f;
            } else if (flags & kViewMaterialID) {
                radiance = unpackUnorm4x8ToFloat(hash(materialID)).xyz;
            } else if (flags & kViewPosition) {
                radiance = p;
            }
            break;
        }
        // -- ^^ Debug View
        // Emissive
        radiance += throughput * emissive;
        // Lights
        // TODO
        // Importance sample next ray
        float3 v = -ray.Direction;
        // Local space
        quat toLocal = QuatRotateToZ(n);
        quat toWorld = toLocal.inverted();
        float3 vLocal = normalize(toLocal.rotate(v));
        float3 nLocal = float3(0,0,1);
        float2 u = float2(rng.sample(), rng.sample());
        float3 rayLocal;
        // Ensure normals are always front facing
        if (dot(geometricNormal, v) < 0.0f) geometricNormal *= -1;
        if (dot(geometricNormal, n) < 0.0f) n *= -1;
        float PspecIndirect = EvalSpecularBRDFProbability(baseColor, metallic, v, n);
        float alpha = roughness * roughness;
        if (rng.sample() < PspecIndirect){
            // Specular
            float3 hLocal;
            if (alpha <= EPS)
                hLocal = float3(0,0,1); // Perfect reflector
            else
                hLocal = SampleGGXVNDF(vLocal, float2(alpha), u);
            float3 lLocal = reflect(-vLocal, hLocal);
            float HoL = clamp(dot(hLocal, lLocal), EPS, 1.0f);
            float NoL = clamp(dot(nLocal, lLocal), EPS, 1.0f);
            float NoV = clamp(dot(nLocal, vLocal), EPS, 1.0f);
            float NoH = clamp(dot(nLocal, hLocal), EPS, 1.0f);
            float3 specularF0 = SpecularF0(baseColor, metallic);
            float3 F = F_Schlick(HoL, specularF0);
            rayLocal = lLocal;
            throughput *= F * SampleGGXVNDFWeight(alpha, alpha * alpha, NoL, NoV) / PspecIndirect;
        } else {
            // Diffuse
            rayLocal = SampleCosineHemisphere(u);
            // ^^ Samping/BRDF PDF (1/pi) cancels each other out vv
            throughput *= DiffuseReflectance(baseColor, metallic) / (1 - PspecIndirect);
            // Weighted with Specular by fresnel mix
            float3 hLocal = SampleGGXVNDF(vLocal, float2(alpha), u);
            float VoH = clamp(dot(vLocal, hLocal), EPS, 1.0f);
            float3 specularF0 = SpecularF0(baseColor, metallic);
            float3 F = F_Schlick(VoH, specularF0);
            throughput *= float3(1) - F;
        }
        ray.Direction = normalize(toWorld.rotate(rayLocal));
        ray.Origin = OffsetRay(p, geometricNormal);
        if (dot(LUMINANCE_RGB,throughput) < EPS)
            break; // No contribution
        if (dot(ray.Direction, geometricNormal) < 0)
            break; // Terminate if ray goes below the surface
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
