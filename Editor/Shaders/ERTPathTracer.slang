// References
// - PBRT v4
// - RAY TRACING GEMS II - Chapter 14
// - https://github.com/boksajak/referencePT/blob/master/shaders/PathTracer.hlsl
// - https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang
import IFlags;
import ICommon;
import ILight;
#include "IBSDF.slang"
uniform UBO globalParams;

// -- Scene
RaytracingAccelerationStructure AS;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitives;
StructuredBuffer<GSMaterial> materials;
SamplerState textureSampler;
RWTexture2D<float4> accumulation;
RWTexture2D<float4> lighting;
[[vk_binding(0,2)]] Texture2D<float4> textures[];

void IntersectTriangle(uint meshOffset, uint primitive, float2 bary, out float3 position, out float3 normal, out float3 geometryNormal, out float3 tangent, out float2 uv, out float bitangentSign){
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
    float3 e20 = p2 - p0, e10 = p1 - p0;
    geometryNormal = normalize(cross(e10, e20));
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
static const uint32_t INSTANCE_NO_HIT = ~0u;
struct HitPayload
{
    // Model space
    float3 position;
    float3 normal;
    float3 geometryNormal;
    float4 tangent;
    float2 uv;
    uint32_t instance;
    uint32_t material;

    bool IsHit() { return instance != INSTANCE_NO_HIT; }
};
struct ShadowPayload
{
    bool hit;
};
struct Payload {
    float3 position;
    float3 normal;
    float3 tangent;
    float3 bitangent;
    float3 geometryNormal;
    uint materialID;
    float3 baseColor;
    float3 emissive;
    float metallic;
    float roughness;
    float transmission;
    float ior;
};
Payload SamplePayload(HitPayload payload)
{
    if (!payload.IsHit()) // Misuse
        return {};
    GSInstance inst = instances[NonUniformResourceIndex(payload.instance)];
    Payload res;
    res.materialID = inst.materialIndex;
    GSMaterial material = materials[NonUniformResourceIndex(res.materialID)];
    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    SampleMaterials(material, payload.uv, baseColor4, normalTBN, pbrMetalRough, emissive4);
    res.baseColor = baseColor4.xyz;
    res.emissive = emissive4.xyz;
    res.roughness = pbrMetalRough.y;
    res.metallic = pbrMetalRough.z;
    res.normal = normalize(payload.normal);
    res.tangent = normalize(payload.tangent.xyz);
    res.geometryNormal = normalize(payload.geometryNormal);
    res.normal = inst.rotation.rotate(res.normal);
    res.tangent = inst.rotation.rotate(res.tangent);
    res.geometryNormal = inst.rotation.rotate(res.geometryNormal);
    res.position = inst.rotation.rotate(payload.position);
    res.position = res.position * inst.scale + inst.transform;
    res.bitangent = cross(res.normal, res.tangent) * (payload.tangent.w > 0.0f ? 1 : -1);
    res.normal = normalize(res.tangent * normalTBN.x + res.bitangent * normalTBN.y + res.normal * normalTBN.z);
    res.ior = 1.5f; // TODO
    res.transmission = material.transmissionFactor;
    return res;
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
// Primary ray with jitter for AA
float3 GeneratePrimaryRay(uint2 pixel, PCG rng)
{
    float2 jitter = lerp(float2(-0.5f), float2(0.5f), rng.sample2D());
    float2 uv = float2(pixel + jitter + 0.5f) / DispatchRaysDimensions().xy;
    float4 ndcPosition = float4(uv, 1.0f, 1.0f);
    ndcPosition.y = 1 - ndcPosition.y;
    ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
    float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);
    return normalize(wsPosition.xyz / wsPosition.w - globalParams.camPosition);
}
// -- Main Shader Routines
bool TraceShadowRay(float3 origin, float3 direction)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.0f;
    ray.TMax = INF;
    ShadowPayload payload;
    TraceRay(AS, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 1, 1, 1, ray, payload);
    return payload.hit;
}

[[SpecializationConstant]] int flags;
[shader("raygeneration")]
void RayGeneration()
{
    uint2 pix = DispatchRaysIndex().xy;
    PCG rng = PCG(pix, globalParams.frameNumber);
    float3 L = float3(0);
    float3 beta = float3(1);
    // Initial ray with jitter for AA
    RayDesc ray;
    ray.Origin = globalParams.camPosition;
    ray.Direction = GeneratePrimaryRay(pix, rng);
    ray.TMin = 0.0f;
    ray.TMax = INF;
    // Simple Physical Lens
    // https://www.pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models#fragment-Computepointonplaneoffocus-0
    // https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang#L922
    if (globalParams.focalDistance > EPS){
        float3 focalPoint = globalParams.focalDistance * ray.Direction;
        // Sample point on lens in world space
        float3 T = mul(globalParams.inverseView, float4(1,0,0,0)).xyz;
        float3 B = mul(globalParams.inverseView, float4(0,1,0,0)).xyz;
        float2 lensSample = SampleConcentricDisk(rng.sample2D()) * globalParams.aperture * rng.sample();
        float3 lensPoint = lensSample.x * T + lensSample.y * B;
        ray.Origin += lensPoint;
        ray.Direction = normalize(focalPoint - lensPoint);
    }
    // Combine the miss hit with the last light sample
    float lastSamplePDF = 0.0f;
    bool isInside = false;
    for (uint bounce = 0; bounce < globalParams.ptMaxBounces; bounce++){
        HitPayload payload;
        TraceRay(AS, 0, 0xFF, 0, 0, 0, ray, payload);
        // Outgoing direction
        float3 view = -ray.Direction;
        UniformInfiniteLight env = UniformInfiniteLight(globalParams.ambientColor, 1.0f);
        if (!payload.IsHit()){
            LightSampleContext ctx; /* UNUSED */
            // Miss - environment
            if (bounce == 0){
                L += env.Le(-view);
                break;
            }
            // Final miss, weight by last sample
            float pdf = env.PDF_Li(ctx, -view, true);
            // We can arrive at a environment sample either from the direct sample path before,
            // or the environment itself.
            float mis = lastSamplePDF > EPS ? BalanceHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
            L += beta * env.Le(-view) * mis;
            break;
        }
        Payload hit = SamplePayload(payload);        
        // Debug View
        if (flags & (kViewBaseColor | kViewNormal | kViewMaterialID | kViewPosition)) {
            if (flags & kViewBaseColor) {
                L = hit.baseColor;
            } else if (flags & kViewNormal) {
                L = hit.normal * 0.5f + 0.5f;
            } else if (flags & kViewMaterialID) {
                L = unpackUnorm4x8ToFloat(hash(hit.materialID)).xyz;
            } else if (flags & kViewPosition) {
                L = hit.position;
            }
            break;
        }
        /** Path Integrator **/
        // Local frame setup
        quat toLocal = quatRotateToZ(hit.normal);
        quat toWorld     = toLocal.inverted();
        float3 n = float3(0,0,1);
        float3 wo = toLocal.rotate(view);
        float eta = isInside ? hit.ior : 1/hit.ior;
        IBxDF bsdf = PrincipledBSDF(hit.baseColor, hit.metallic, hit.roughness, eta, hit.transmission, true);
        // Light sampling [TODO. Really read into this chapter once my tests are done with...]
        // https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling
        {
            LightSampleContext ctx = LightSampleContext(hit.position, hit.geometryNormal, hit.normal);
            LightLiSample Li = LightLiSample();
            SampledSpectrum radiance = 0;
            {
                DistantLight sun = DistantLight(SampledSpectrum(globalParams.sunIntensity), globalParams.sunDirection);
                // UniformInfiniteLight env [defined above]
                // We only have the sun and the ambient light. For uniform MIS these have equal weights.
                float pSun = AnyLuminance(globalParams.sunIntensity) ? 0.5f : 0.0f;
                float pEnv = 1.0f - pSun;
                bool sampleSun = rng.sample() < pSun;

                float samplePDF, pSample, otherPDF;
                if (sampleSun){
                    Li = sun.Sample_Li(ctx, rng.sample2D());
                    radiance = Li.Li / (Li.pdf * pSun);
                    otherPDF = env.PDF_Li(ctx, Li.wi, true);
                    samplePDF = Li.pdf;
                    pSample = pSun;
                } else {
                    Li = env.Sample_Li(ctx, rng.sample2D());
                    radiance = Li.Li / (Li.pdf * pEnv);
                    otherPDF = sun.PDF_Li(ctx, Li.wi, true);
                    samplePDF = Li.pdf;
                    pSample = pEnv;
                }
                // https://www.pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency#MultipleImportanceSampling
                float mis = BalanceHeuristic(1, samplePDF, 1, otherPDF);
                radiance *= mis;
                // vvv Similar to PrincipledBSDF's PDF calculation
                // You can derive these from single sample situations
                Li.pdf = mix(otherPDF, samplePDF, pSample);
                // AO and Direct shadow ray
                float3 shadowNormalFF = dot(Li.wi, hit.geometryNormal) < 0.0f ? -hit.geometryNormal : hit.geometryNormal;
                float3 shadowOrigin = OffsetRay(hit.position, shadowNormalFF);
                bool shadowed = TraceShadowRay(shadowOrigin, Li.wi);
                if (shadowed)
                    Li.pdf = 0.0f;
            }
            // Eval and add light contribution
            // Li is now effectively the 'combined' sample here.
            float3 wi = toLocal.rotate(Li.wi);
            float3 f = bsdf.f(wo, wi) * AbsCosTheta(wi);
            float pdf = bsdf.PDF(wo, wi);
            // Light can arrive from the light itself, or the sampled BSDF path
            if (Li.pdf > EPS && pdf > EPS){
                float mis = BalanceHeuristic(1, Li.pdf, 1, pdf);
                L += beta * f * radiance * mis;
            }
        }
        if (flags & kViewPTDirect)
            break;
        // Emissive
        L += beta * hit.emissive;
        BSDFSample sample = bsdf.Sample_f(wo, rng.sample(), rng.sample2D());
        if (!sample.IsValid()) {
            L = float3(1, 0, 0);
            break;
        }
        lastSamplePDF = sample.pdf;
        beta *= sample.f * AbsCosTheta(sample.wi) / sample.pdf;
        ray.Direction = toWorld.rotate(sample.wi);
        float3 rayNormal = dot(ray.Direction, hit.geometryNormal) < 0.0f ? -hit.geometryNormal : hit.geometryNormal;
        ray.Origin = OffsetRay(hit.position, rayNormal);
        if (IsTransmission(sample.flags))
            isInside = !isInside;
        // Russian Roulette
        float rrPCount = min(MaxComponent(beta) + 0.01f, 0.95f);
        if (rng.sample() > rrPCount) // Contribution is low, may terminate early
            break;
        beta /= rrPCount; // Boost non-terminated paths
    }
    // Transform world space point
    // Accumulation
    float4 output = float4(L, 1.0f);
    float frameCount = float(globalParams.ptAccumualatedFrames + 1);
    if (frameCount == 1){
        accumulation[pix] = lighting[pix] = output;
    } else {
        accumulation[pix] += output;
        lighting[pix] = accumulation[pix] / frameCount;
    }
}
// Naive stochastic opacity map implementation
// This could get a LOT more involved with ray differentials for sampling
// (our engine provides full mip chains for all textures - use it!)
// And as of today we got Opacity Micro Maps for acceleration too.
//  https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html
//  https://devblogs.microsoft.com/directx/omm/
// ...This will do for now until I get more time (winter vacation!).
// Final week is coming soon - I have yet to prepare for any of my exams :/
bool AnyHitOpacityIgnore(inout PCG rng, uint instance, uint primitive, BuiltInTriangleIntersectionAttributes attr, bool isShadowRay = false){
    GSInstance inst = instances[NonUniformResourceIndex(instance)];
    GSMaterial material = materials[NonUniformResourceIndex(inst.materialIndex)];
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTexture != 0){
        HitPayload payload;
        IntersectTriangle(inst.meshOffset, primitive, attr.barycentrics.xy, payload.position, payload.normal, payload.geometryNormal, payload.tangent.xyz, payload.uv, payload.tangent.w);
        baseColor *= textures[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(textureSampler, payload.uv, 0);
    }
    float opacity = baseColor.a;

    if (opacity == 1.0f)
        return false;

    if (rng.sample() < 1.0f - opacity)
        return true;
    if (isShadowRay && rng.sample() < material.transmissionFactor)
        return true;
    return false;
}
[shader("anyhit")] // Hit Group 0
void RayOpacityAnyHit(in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceIndex();
    uint primitive = PrimitiveIndex();
    PCG rng = PCG(DispatchRaysIndex().xy, globalParams.frameNumber, hash(uint2(instance, primitive)));
    if (AnyHitOpacityIgnore(rng, instance, primitive, attr, false))
        return IgnoreHit();
}
[shader("anyhit")] // Hit Group 1
void ShadowRayAnyHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceIndex();
    uint primitive = PrimitiveIndex();
    PCG rng = PCG(DispatchRaysIndex().xy, globalParams.frameNumber, hash(uint2(instance, primitive)));
    if (AnyHitOpacityIgnore(rng, instance, primitive, attr, true /* shadow transmission */))
        return IgnoreHit();
    payload.hit = true;
    AcceptHitAndEndSearch();
}

[shader("closesthit")] // Hit Group 0
void RayClosestHit(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceIndex();
    GSInstance inst = instances[NonUniformResourceIndex(instance)];
    GSMaterial material = materials[NonUniformResourceIndex(inst.materialIndex)];
    IntersectTriangle(inst.meshOffset, PrimitiveIndex(), attr.barycentrics.xy, payload.position, payload.normal, payload.geometryNormal, payload.tangent.xyz, payload.uv, payload.tangent.w);
    payload.instance = instance;
    payload.material = inst.materialIndex;
}

[shader("miss")] // Miss 0
void RayMiss(inout HitPayload payload)
{
    payload.instance = INSTANCE_NO_HIT;
}
[shader("miss")] // Miss 1
void ShadowRayMiss(inout ShadowPayload payload)
{
    payload.hit = false;
}
