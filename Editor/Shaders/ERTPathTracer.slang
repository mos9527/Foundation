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
// -- Per-lobe AOV buffers (Diffuse/Specular/Transmission × Direct/Indirect)
RWTexture2D<float4> accumDiffuseDirect;
RWTexture2D<float4> accumDiffuseIndirect;
RWTexture2D<float4> accumSpecularDirect;
RWTexture2D<float4> accumSpecularIndirect;
RWTexture2D<float4> accumTransDirect;
RWTexture2D<float4> accumTransIndirect;
RWTexture2D<float4> aovDiffuseDirect;
RWTexture2D<float4> aovDiffuseIndirect;
RWTexture2D<float4> aovSpecularDirect;
RWTexture2D<float4> aovSpecularIndirect;
RWTexture2D<float4> aovTransDirect;
RWTexture2D<float4> aovTransIndirect;
[[vk_binding(0,2)]] Texture2D<float4> textures[];

// World-space hit geometry
struct HitState
{
    float3 position;
    float3 normal;
    float3 geometryNormal;
    float3 tangent;
    float3 bitangent;
    float2 uv;
};

HitState getHitState(uint meshOffset, uint primitive, float2 bary, GSInstance inst, float3 rayOrigin, float3 rayDirection, float hitT)
{
    HitState hit;

    // Construct position from ray equation (more robust than interpolation + transform)
    hit.position  = rayOrigin + rayDirection * hitT;

    // Barycentric weights
    const float3 barycentrics = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

    // Fetch triangle indices
    GSMesh mesh = primitives.Load<GSMesh>(meshOffset);
    uint i0 = primitives.Load<uint32_t>(mesh.idxOffset + (primitive * 3 + 0) * sizeof(uint32_t));
    uint i1 = primitives.Load<uint32_t>(mesh.idxOffset + (primitive * 3 + 1) * sizeof(uint32_t));
    uint i2 = primitives.Load<uint32_t>(mesh.idxOffset + (primitive * 3 + 2) * sizeof(uint32_t));

    // Fetch and decode vertices
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

    // Interpolate vertex attributes (model space)
    const float3 normal         = normalize(n0 * barycentrics.x + n1 * barycentrics.y + n2 * barycentrics.z);
    const float3 tangent        = normalize(t0 * barycentrics.x + t1 * barycentrics.y + t2 * barycentrics.z);
    const float  bitangentSign  = bi0 * barycentrics.x + bi1 * barycentrics.y + bi2 * barycentrics.z;
    hit.uv                      = u0 * barycentrics.x + u1 * barycentrics.y + u2 * barycentrics.z;

    // Geometry normal from triangle edges
    const float3 geoNormal = normalize(cross(p1 - p0, p2 - p0));

    // Transform normals/tangents to world space via instance TRS
    hit.normal         = normalize(inst.rotation.rotate(normal / inst.scale));
    hit.tangent        = normalize(inst.rotation.rotate(tangent / inst.scale));
    hit.geometryNormal = normalize(inst.rotation.rotate(geoNormal / inst.scale));
    hit.bitangent      = cross(hit.normal, hit.tangent) * (bitangentSign > 0.0f ? 1 : -1);

    return hit;
}

void SampleMaterials(GSMaterial material, float2 uv,
    out float4 baseColor, out float4 normalTBN, out float4 pbrMetalRough, out float4 emissive)
{
    baseColor = material.baseColorFactor;
    if (material.baseColorTexture != ~0u)
        baseColor *= textures[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(textureSampler, uv, 0);
    normalTBN = float4(0, 0, 1, 0);
    if (material.normalTexture != ~0u)
        normalTBN = textures[NonUniformResourceIndex(material.normalTexture)].SampleLevel(textureSampler, uv, 0) * 2 - float4(1);
    pbrMetalRough = float4(1, material.roughnessFactor, material.metallicFactor, 1);
    if (material.metallicRoughnessTexture != ~0u)
        pbrMetalRough *= textures[NonUniformResourceIndex(material.metallicRoughnessTexture)].SampleLevel(textureSampler, uv, 0);
    emissive = float4(material.emissiveFactor, 0);
    if (material.emissiveTexture != ~0u)
        emissive *= textures[NonUniformResourceIndex(material.emissiveTexture)].SampleLevel(textureSampler, uv, 0);
}

static const uint32_t INSTANCE_NO_HIT = ~0u;

struct HitPayload
{
    uint32_t instance;
    uint32_t primitive;
    float2 bary;
    float hitT;

    bool IsHit() { return instance != INSTANCE_NO_HIT; }
};

struct ShadowPayload
{
    bool hit;
};

struct Payload
{
    HitState hit;
    uint materialID;
    float3 baseColor;
    float3 emissive;
    float metallic;
    float roughness;
    float transmission;
    float ior;
};
Payload SamplePayload(HitPayload payload, RayDesc ray)
{
    if (!payload.IsHit()) // Misuse
        return {};
    GSInstance inst = instances[NonUniformResourceIndex(payload.instance)];
    Payload res;
    res.materialID = inst.materialIndex;
    res.hit = getHitState(inst.meshOffset, payload.primitive, payload.bary, inst, ray.Origin, ray.Direction, payload.hitT);
    GSMaterial material = materials[NonUniformResourceIndex(res.materialID)];
    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    SampleMaterials(material, res.hit.uv, baseColor4, normalTBN, pbrMetalRough, emissive4);
    res.baseColor = baseColor4.xyz;
    res.emissive = emissive4.xyz;
    res.roughness = pbrMetalRough.y;
    res.metallic = pbrMetalRough.z;
    // Apply normal map in tangent space
    res.hit.normal = normalize(res.hit.tangent * normalTBN.x + res.hit.bitangent * normalTBN.y + res.hit.normal * normalTBN.z);
    res.ior = material.ior;
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

// 辅助：将贡献累加到对应的 AOV 类别
void AddToAOV(AOVCategory cat, bool isDirect,
              inout float3 dd, inout float3 di,
              inout float3 sd, inout float3 si,
              inout float3 td, inout float3 ti,
              float3 contrib) {
    if (cat == AOVCategory::Diffuse) {
        if (isDirect) dd += contrib; else di += contrib;
    } else if (cat == AOVCategory::Transmission) {
        if (isDirect) td += contrib; else ti += contrib;
    } else {
        if (isDirect) sd += contrib; else si += contrib;
    }
}

[[SpecializationConstant]] int flags;
[shader("raygeneration")]
void RayGeneration()
{
    uint2 pix = DispatchRaysIndex().xy;
    PCG rng = PCG(pix, globalParams.frameNumber);
    float3 L = float3(0);
    float3 beta = float3(1);
    // Per-lobe AOV accumulators
    float3 LDiffuseDirect   = float3(0);
    float3 LDiffuseIndirect = float3(0);
    float3 LSpecDirect      = float3(0);
    float3 LSpecIndirect    = float3(0);
    float3 LTransDirect     = float3(0);
    float3 LTransIndirect   = float3(0);
    // 首次弹射分类（bounce > 0 的间接贡献归属）
    AOVCategory firstBounceCategory = AOVCategory::Diffuse;
    // Initial ray with jitter for AA
    RayDesc ray;
    ray.Origin = globalParams.camPosition;
    ray.Direction = GeneratePrimaryRay(pix, rng);
    ray.TMin = 0.0f;
    ray.TMax = INF;
    // Simple Physical Lens
    // https://www.pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models#fragment-Computepointonplaneoffocus-0
    // https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang#L922
    if (globalParams.focalDistance > EPS) {
        float3 focalPoint = globalParams.focalDistance * ray.Direction;
        // Sample point on lens in world space
        float3 T = mul(globalParams.inverseView, float4(1, 0, 0, 0)).xyz;
        float3 B = mul(globalParams.inverseView, float4(0, 1, 0, 0)).xyz;
        float2 lensSample = SampleConcentricDisk(rng.sample2D()) * globalParams.aperture * rng.sample();
        float3 lensPoint = lensSample.x * T + lensSample.y * B;
        ray.Origin += lensPoint;
        ray.Direction = normalize(focalPoint - lensPoint);
    }
    // Combine the miss hit with the last light sample
    float lastSamplePDF = 0.0f;
    // Scene lights
    DistantLight sun = DistantLight(SampledSpectrum(globalParams.sunIntensity), globalParams.sunDirection);
    // Unified infinite light — falls back to constant color when no env map is bound
    InfiniteLight env = InfiniteLight(
        SampledSpectrum(globalParams.ambientColor),
        globalParams.envMapScale,
        globalParams.useEnvMap
    );
    LightMixture light = LightMixture();
    for (uint bounce = 0; bounce < globalParams.ptMaxBounces; bounce++) {
        HitPayload payload;
        TraceRay(AS, 0, 0xFF, 0, 0, 0, ray, payload);
        // Outgoing direction
        float3 view = -ray.Direction;
        if (AnyLuminance(globalParams.sunIntensity))
            light.AddLight(sun, 0.5f);
        light.AddLight(env, 0.5f);
        light.Finalize();
        if (!payload.IsHit()) {
            LightSampleContext ctx; /* UNUSED */
            // Miss — environment
            if (bounce == 0) {
                float3 envLe = light.Le(-view);
                L += envLe;
                // 环境光未经表面，不归入任何 AOV lobe
                break;
            }
            // Final miss, MIS-weight against the last BSDF sample
            float pdf = light.PDF_Li(ctx, -view);
            float mis = lastSamplePDF > EPS ? BalanceHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
            float3 contrib = beta * light.Le(-view) * mis;
            L += contrib;
            // Bounce > 0 的环境光视为 indirect，归属首次弹射类别
            AddToAOV(firstBounceCategory, false,
                     LDiffuseDirect, LDiffuseIndirect,
                     LSpecDirect, LSpecIndirect,
                     LTransDirect, LTransIndirect, contrib);
            break;
        }
        Payload surface = SamplePayload(payload, ray);

        // Debug View
        if (flags & (kViewBaseColor | kViewNormal | kViewMaterialID | kViewPosition)) {
            if (flags & kViewBaseColor) {
                L = surface.baseColor;
            } else if (flags & kViewNormal) {
                L = surface.hit.normal * 0.5f + 0.5f;
            } else if (flags & kViewMaterialID) {
                L = unpackUnorm4x8ToFloat(hash(surface.materialID)).xyz;
            } else if (flags & kViewPosition) {
                L = surface.hit.position;
            }
            break;
        }
        // -- Path Integrator
        // Local frame setup
        quat toLocal = quatRotateToZ(surface.hit.normal);
        quat toWorld = toLocal.inverted();
        float3 wo = toLocal.rotate(view);
        // 构建各独立 lobe 及其权重
        PrincipledBSDFLobes bsdf = PrincipledBSDFLobes(
            surface.baseColor, surface.metallic, surface.roughness,
            surface.ior, surface.transmission, true, wo);
        // Light sampling via LightMixture (NEE / Direct Lighting)
        // https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling
        {
            LightSampleContext ctx = LightSampleContext(surface.hit.position, surface.hit.geometryNormal, surface.hit.normal);
            // Sample_Li returns: radiance = Li/(pdf*pChoice), pdf = mixture PDF
            LightLiSample Li = light.Sample_Li(ctx, rng.sample(), rng.sample2D());
            SampledSpectrum radiance = Li.Li;
            // Shadow ray
            if (Li.pdf > EPS) {
                float3 shadowNormalFF = dot(Li.wi, surface.hit.geometryNormal) < 0.0f ? surface.hit.geometryNormal : surface.hit.geometryNormal;
                float3 shadowOrigin = OffsetRay(surface.hit.position, shadowNormalFF);
                bool shadowed = TraceShadowRay(shadowOrigin, Li.wi);
                if (shadowed)
                    Li.pdf = 0.0f;
            }
            // Eval and add light contribution
            float3 wi = toLocal.rotate(Li.wi);
            float bsdfPdf = bsdf.PDF(wo, wi, TransportMode::Radiance, BxDFReflTransFlags::All);
            // MIS between light sampling path and BSDF sampling path
            if (Li.pdf > EPS && bsdfPdf > EPS) {
                float mis = BalanceHeuristic(1, Li.pdf, 1, bsdfPdf);
                if (bounce == 0) {
                    // 首次弹射：逐 lobe 拆分直接光
                    float3 fD, fS, fT;
                    bsdf.f_split(wo, wi, TransportMode::Radiance, fD, fS, fT);
                    float3 scale = beta * AbsCosTheta(wi) * radiance * mis;
                    float3 cD = fD * scale;
                    float3 cS = fS * scale;
                    float3 cT = fT * scale;
                    LDiffuseDirect += cD;
                    LSpecDirect    += cS;
                    LTransDirect   += cT;
                    L += cD + cS + cT;
                } else {
                    // 后续弹射：全部直接光归属首次弹射类别（indirect）
                    float3 f = bsdf.f(wo, wi, TransportMode::Radiance) * AbsCosTheta(wi);
                    float3 contrib = beta * f * radiance * mis;
                    L += contrib;
                    AddToAOV(firstBounceCategory, false,
                             LDiffuseDirect, LDiffuseIndirect,
                             LSpecDirect, LSpecIndirect,
                             LTransDirect, LTransIndirect, contrib);
                }
            }
        }
        if (flags & kViewPTDirect)
            break;
        // Emissive
        {
            float3 emContrib = beta * surface.emissive;
            L += emContrib;
            if (bounce == 0) {
                // 首次弹射 emissive 归入 diffuse direct
                LDiffuseDirect += emContrib;
            } else {
                AddToAOV(firstBounceCategory, false,
                         LDiffuseDirect, LDiffuseIndirect,
                         LSpecDirect, LSpecIndirect,
                         LTransDirect, LTransIndirect, emContrib);
            }
        }
        // BSDF 采样（间接光路径）
        AOVCategory sampleCategory;
        BSDFSample sample = bsdf.Sample_f(wo, rng.sample(), rng.sample2D(),
                                          TransportMode::Radiance, BxDFReflTransFlags::All,
                                          sampleCategory);
        if (!sample.IsValid()) {
            break;
        }
        // 首次弹射记录 lobe 类别
        if (bounce == 0) {
            firstBounceCategory = sampleCategory;
        }
        lastSamplePDF = sample.pdf;
        beta *= sample.f * AbsCosTheta(sample.wi) / sample.pdf;
        if (any(isnan(beta)) || any(isinf(beta)))
            break;
        ray.Direction = toWorld.rotate(sample.wi);

        float3 rayNormal = dot(ray.Direction, surface.hit.geometryNormal) < 0.0f ? -surface.hit.geometryNormal : surface.hit.geometryNormal;
        ray.Origin = OffsetRay(surface.hit.position, rayNormal);
        // Russian Roulette (after 2nd bounce)
        if (bounce >= 2) {
            float rrPCount = min(MaxComponent(beta) + 0.01f, 0.95f);
            if (rng.sample() > rrPCount) // Contribution is low, may terminate early
                break;
            beta /= rrPCount; // Boost non-terminated paths
        }
    }
    // Sanitize
    if (any(isnan(L)) || any(isinf(L))) L = float3(0.0f);
    if (any(isnan(LDiffuseDirect))   || any(isinf(LDiffuseDirect)))   LDiffuseDirect   = float3(0);
    if (any(isnan(LDiffuseIndirect))  || any(isinf(LDiffuseIndirect)))  LDiffuseIndirect  = float3(0);
    if (any(isnan(LSpecDirect))      || any(isinf(LSpecDirect)))      LSpecDirect      = float3(0);
    if (any(isnan(LSpecIndirect))    || any(isinf(LSpecIndirect)))    LSpecIndirect    = float3(0);
    if (any(isnan(LTransDirect))     || any(isinf(LTransDirect)))     LTransDirect     = float3(0);
    if (any(isnan(LTransIndirect))   || any(isinf(LTransIndirect)))   LTransIndirect   = float3(0);

    float4 output = float4(L, 1.0f);
    float4 outDD = float4(LDiffuseDirect, 1.0f);
    float4 outDI = float4(LDiffuseIndirect, 1.0f);
    float4 outSD = float4(LSpecDirect, 1.0f);
    float4 outSI = float4(LSpecIndirect, 1.0f);
    float4 outTD = float4(LTransDirect, 1.0f);
    float4 outTI = float4(LTransIndirect, 1.0f);

    // Accumulation
    float frameCount = float(globalParams.ptAccumualatedFrames + 1);
    if (frameCount == 1) {
        accumulation[pix] = lighting[pix] = output;
        accumDiffuseDirect[pix]   = aovDiffuseDirect[pix]   = outDD;
        accumDiffuseIndirect[pix] = aovDiffuseIndirect[pix] = outDI;
        accumSpecularDirect[pix]  = aovSpecularDirect[pix]  = outSD;
        accumSpecularIndirect[pix]= aovSpecularIndirect[pix]= outSI;
        accumTransDirect[pix]     = aovTransDirect[pix]     = outTD;
        accumTransIndirect[pix]   = aovTransIndirect[pix]   = outTI;
    } else {
        accumulation[pix] += output;
        lighting[pix] = accumulation[pix] / frameCount;
        accumDiffuseDirect[pix]   += outDD;
        accumDiffuseIndirect[pix] += outDI;
        accumSpecularDirect[pix]  += outSD;
        accumSpecularIndirect[pix]+= outSI;
        accumTransDirect[pix]     += outTD;
        accumTransIndirect[pix]   += outTI;
        aovDiffuseDirect[pix]   = accumDiffuseDirect[pix]   / frameCount;
        aovDiffuseIndirect[pix] = accumDiffuseIndirect[pix] / frameCount;
        aovSpecularDirect[pix]  = accumSpecularDirect[pix]  / frameCount;
        aovSpecularIndirect[pix]= accumSpecularIndirect[pix]/ frameCount;
        aovTransDirect[pix]     = accumTransDirect[pix]     / frameCount;
        aovTransIndirect[pix]   = accumTransIndirect[pix]   / frameCount;
    }
}
// Naive stochastic opacity map implementation
// This could get a LOT more involved with ray differentials for sampling
// (our engine provides full mip chains for all textures - use it!)
// And as of today we got Opacity Micro Maps for acceleration too.
//  https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html
//  https://devblogs.microsoft.com/directx/omm/
bool AnyHitOpacityIgnore(inout PCG rng, uint instance, uint primitive,
    BuiltInTriangleIntersectionAttributes attr, bool isShadowRay = false)
{
    GSInstance inst = instances[NonUniformResourceIndex(instance)];
    GSMaterial material = materials[NonUniformResourceIndex(inst.materialIndex)];
    float4 baseColor = material.baseColorFactor;
    if (material.baseColorTexture != ~0u) {
        HitState hs = getHitState(inst.meshOffset, primitive, attr.barycentrics.xy, inst, WorldRayOrigin(), WorldRayDirection(), RayTCurrent());
        baseColor *= textures[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(textureSampler, hs.uv, 0);
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
    payload.instance = InstanceIndex();
    payload.primitive = PrimitiveIndex();
    payload.bary = attr.barycentrics.xy;
    payload.hitT = RayTCurrent();
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
