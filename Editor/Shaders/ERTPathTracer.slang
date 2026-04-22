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

struct Alias {
    float prob, select;
    uint alias;
}
// -- Scene
RaytracingAccelerationStructure AS;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitives;
StructuredBuffer<GSMaterial> materials;
StructuredBuffer<GSLight> lights;
StructuredBuffer<Alias> lightAliasTable;
SamplerState textureSampler;
StructuredBuffer<uint> sobolMatrices;
// -- Accumulation buffers (Welford online mean)
RWTexture2D<float4> gBuffer0;  // BaseColor.rgb, MaterialID (packed)
RWTexture2D<float4> gBuffer1;  // Normal.xyz * 0.5 + 0.5, 0
RWTexture2D<float4> gBuffer2;  // Position.xyz, 0
RWTexture2D<float4> diffuse;   // Diffuse lobe radiance
RWTexture2D<float4> specular;  // Specular lobe radiance (incl. transmission)
RWTexture2D<uint32_t> pickIDBuffer; // Instance ID map, ~0u = no object (written on primary hit only)
[[vk_binding(0,2)]] Texture2D<float4> textures[];

// Number of SER coherence hint bits — enough to cover all materialID values
// 8 bits is sufficient for 256 materials, consistent with GBuffer packing
static const uint kSERCoherenceHintBits = 8u;
// Capability flags. Used with SER only for now.
[[SpecializationConstant]] int capabilityFlags;

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

uint GetInstanceBufferIndex(uint localInstanceIndex)
{
    return globalParams.firstInstance + localInstanceIndex;
}

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
    hit.normal         = normalize(inst.rotation.rotate(normal));
    hit.tangent        = normalize(inst.rotation.rotate(tangent));
    hit.geometryNormal = normalize(inst.rotation.rotate(geoNormal));
    hit.bitangent      = cross(hit.normal, hit.tangent) * (bitangentSign > 0.0f ? 1 : -1);
    if (dot(hit.geometryNormal, rayDirection) > 0.0f)
        hit.geometryNormal = -hit.geometryNormal;
    if (dot(hit.geometryNormal, hit.normal) < 0.0f)
        hit.normal = -hit.normal;
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
    GSInstance inst = instances[NonUniformResourceIndex(GetInstanceBufferIndex(payload.instance))];
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

void AddRadiance(inout float3 L, float3 v, bool direct = false)
{
    if (any(isnan(v)) || any(isinf(v)))
        return;
    v = max(float3(0.0f), v);
    if (!direct){
        // Firefly clamp — cap per-sample contribution to suppress extreme outliers
        float maxComp = MaxComponent(v);
        if (maxComp > globalParams.ptFireflyClamp)
            v *= globalParams.ptFireflyClamp / maxComp;
    }
    L += v;
}

// Primary ray with jitter for AA
float3 GeneratePrimaryRay<S : ISampler>(uint2 pixel, inout S rng)
{
    float2 jitter = lerp(float2(-0.5f), float2(0.5f), rng.sample2D());
    float2 uv = float2(pixel + jitter + 0.5f) / DispatchRaysDimensions().xy;
    float4 ndcPosition = float4(uv, 1.0f, 1.0f);
    ndcPosition.y = 1 - ndcPosition.y;
    ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
    float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);
    return normalize(wsPosition.xyz / wsPosition.w - globalParams.camPosition.xyz);
}

// -- Main Shader Routines

// TraceRay via HitObject — returns a hit object without invoking closest hit.
// Used by SER to reorder threads before shading.
HitObject TraceHitObject(RayDesc ray)
{
    HitPayload payload;
    return HitObject.TraceRay(AS, 0, 0xFF, 0, 0, 0, ray, payload);
}

// -- Light Sampling

bool TraceShadowRay(float3 origin, float3 direction, float tMax = INF)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = EPS_TMin;
    ray.TMax = tMax;
    ShadowPayload payload;
    TraceRay(AS, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, 1, 1, 1, ray, payload);
    return payload.hit;
}

float GetEnvLightSelectionWeight()
{
    SampledSpectrum Lemit = SampledSpectrum(globalParams.ambientPower);
    float w = dot(LUMINANCE_RGB, Lemit);
    if (globalParams.useEnvMap != 0)
        w *= globalParams.envMapScale;
    return w;
}

LightLiSample SampleSceneLight(GSLight gl, LightSampleContext ctx, float2 u)
{
    SampledSpectrum Lemit = SampledSpectrum(gl.color * gl.power);
    switch (gl.type) {
    case 0: {
        DistantLight light = DistantLight(Lemit, gl.direction);
        return light.Sample_Li(ctx, u);
    }
    case 1: {
        PointLight light = PointLight(Lemit, gl.position);
        return light.Sample_Li(ctx, u);
    }
    case 2: {
        SpotLight light = SpotLight(Lemit, gl.position, gl.direction,
            acos(gl.spotOuterCosAngle), acos(gl.spotInnerCosAngle));
        return light.Sample_Li(ctx, u);
    }
    case 3: {
        DiskLight light = DiskLight(Lemit, gl.position, gl.dpdu, gl.dpdv, gl.radius, gl.twoSided != 0);
        return light.Sample_Li(ctx, u);
    }
    case 4: {
        RectLight light = RectLight(Lemit, gl.position,
            gl.dpdu, gl.dpdv, gl.twoSided != 0);
        return light.Sample_Li(ctx, u);
    }
    default:
        return LightLiSample();
    }
}

LightLiSample SampleSceneLight(LightSampleContext ctx, InfiniteLight env, float uc, float2 u, float weightSum)
{
    if (weightSum <= 0.0f)
        return LightLiSample();

    float envWeight = GetEnvLightSelectionWeight();
    
    // Decide whether to sample env light or scene light
    if (uc * weightSum < envWeight) {
        LightLiSample Li = env.Sample_Li(ctx, u);
        if (Li.pdf <= 0.0f)
            return LightLiSample();

        float pChoice = envWeight / weightSum;
        SampledSpectrum radiance = Li.Li / (Li.pdf * pChoice);
        float mixPDF = Li.isDelta ? Li.pdf : (Li.pdf * pChoice);
        return LightLiSample(radiance, Li.wi, mixPDF, Li.isDelta, Li.dist);
    }
    
    // Sample scene light using alias table
    if (globalParams.numSceneLights > 0) {
        // Re-scale uc for scene lights
        uc = (uc * weightSum - envWeight) / (weightSum - envWeight);
        
        uint numLights = globalParams.numSceneLights;
        uint idx = min(uint(uc * numLights), numLights - 1);
        Alias bin = lightAliasTable[globalParams.firstLightAliasTable + idx];
        // Get another i.i.d.
        uc = uc * numLights - idx;
        idx = uc >= bin.select ? bin.alias : idx;
        // Final selection
        float prob = lightAliasTable[globalParams.firstLightAliasTable + idx].prob;
        // Draw sample from selected light
        GSLight gl = lights[globalParams.firstLight + idx];
        LightLiSample Li = SampleSceneLight(gl, ctx, u);
        if (Li.pdf > 0.0f) {
            float pSceneLights = (weightSum - envWeight) / weightSum;
            float pChoice = pSceneLights * prob;

            SampledSpectrum radiance = Li.Li / (Li.pdf * pChoice);
            float mixPDF = Li.isDelta ? Li.pdf : (Li.pdf * pChoice);
            return LightLiSample(radiance, Li.wi, mixPDF, Li.isDelta, Li.dist);
        }
    }

    return LightLiSample();
}

[shader("raygeneration")]
void RayGeneration()
{
    uint2 pix = DispatchRaysIndex().xy;

    // PCG or Sobol
    if (globalParams.ptSampler == 0)
    {
        PCG rng = PCG(pix, globalParams.frameNumber);
        PathTrace(pix, rng);
    }
    else
    {
        Sobol rng = Sobol(pix, globalParams.frameNumber, sobolMatrices);
        PathTrace(pix, rng);
    }
}

void PathTrace<S : ISampler>(uint2 pix, inout S rng)
{
    float3 beta = float3(1);
    // Per-lobe AOV accumulators (Diffuse / Specular)
    float3 LDiffuse  = float3(0);
    float3 LSpecular = float3(0);
    AOVCategory firstBounceCategory = AOVCategory::Undefined;
    // GBuffer written on primary hit
    bool gbufferWritten = false;
    float frameCount = float(globalParams.ptAccumulatedFrames + 1);
    // Initial ray with jitter for AA
    RayDesc ray;
    ray.Origin = globalParams.camPosition.xyz;
    ray.Direction = GeneratePrimaryRay(pix, rng);
    ray.TMin = EPS_TMin;
    ray.TMax = INF;
    // Simple Physical Lens
    // https://www.pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models#fragment-Computepointonplaneoffocus-0
    // https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang#L922
    if (globalParams.aperture > EPS) {
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
    // Unified infinite light — falls back to constant color when no env map is bound
    InfiniteLight env = InfiniteLight(
        SampledSpectrum(globalParams.ambientColor * globalParams.ambientPower),
        globalParams.envMapScale,
        globalParams.useEnvMap,
        globalParams.envAzimuthOffset
    );
    float totalLightWeight = globalParams.sceneLightWeightSum + GetEnvLightSelectionWeight();
    LightSampleContext lastLightCtx = LightSampleContext(float3(0), float3(0), float3(0));
    uint diffuseBounces = 0;
    uint specularBounces = 0;
    uint transmissionBounces = 0;
    for (uint bounce = 0; ; bounce++) {
        HitPayload payload;
        if (capabilityFlags != 0 /* SER */) {
            // SER
            // https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder
            // https://docs.vulkan.org/samples/latest/samples/extensions/ray_tracing_invocation_reorder/README.html
            HitObject hitObj = TraceHitObject(ray);
            // Extract materialID from the hit instance as a coherence hint
            // Miss is identified by 0xFFu (distinct from any valid material)
            uint coherenceHint = 0xFFu;
            if (hitObj.IsHit()) {
                GSInstance inst = instances[NonUniformResourceIndex(hitObj.GetInstanceIndex())];
                coherenceHint = inst.materialIndex & 0xFFu;
            }
            ReorderThread(hitObj, coherenceHint, kSERCoherenceHintBits);
            // Invoke closest hit / miss to populate the payload
            HitObject::Invoke(AS, hitObj, payload);
        } else
            TraceRay(AS, 0, 0xFF, 0, 0, 0, ray, payload);
        // Outgoing direction
        float3 view = -ray.Direction;
        if (!payload.IsHit()) {
            // Miss — environment
            if (bounce == 0) {
                // Environment light with no surface hit — classify as specular (background)
                float3 envLe = env.Le(-view);
                AddRadiance(LSpecular, envLe, true);
                break;
            }
            // Final miss, MIS-weight against the last BSDF sample
            float envWeight = GetEnvLightSelectionWeight();
            float pdf = totalLightWeight > 0.0f ? (envWeight / totalLightWeight) * env.PDF_Li(lastLightCtx, -view, true) : 0.0f;
            float mis = lastSamplePDF > EPS ? PowerHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
            float3 contrib = beta * env.Le(-view) * mis;
            // Environment contribution at bounce > 0 is indirect — attribute to first-bounce category
            if (firstBounceCategory == AOVCategory::Diffuse)
                AddRadiance(LDiffuse, contrib, false);
            else
                AddRadiance(LSpecular, contrib, false);
            break;
        }
        Payload surface = SamplePayload(payload, ray);

        // Accumulate GBuffer on primary hit
        if (bounce == 0 && !gbufferWritten) {
            float4 g0 = float4(surface.baseColor, float(surface.materialID) / 255.0f);
            float4 g1 = float4(surface.hit.normal * 0.5f + 0.5f, 0);
            float4 g2 = float4(surface.hit.position, 0);
            if (frameCount == 1) {
                gBuffer0[pix] = g0;
                gBuffer1[pix] = g1;
                gBuffer2[pix] = g2;
            } else {
                float invN = 1.0f / frameCount;
                gBuffer0[pix] += (g0 - gBuffer0[pix]) * invN;
                gBuffer1[pix] += (g1 - gBuffer1[pix]) * invN;
                gBuffer2[pix] += (g2 - gBuffer2[pix]) * invN;
            }
            // R32Uint, so primary hit only.
            pickIDBuffer[pix] = payload.instance;
            gbufferWritten = true;
        }
        // -- Path Integrator
        // Local frame setup
        quat toLocal = quatRotateToZ(surface.hit.normal);
        quat toWorld = toLocal.inverted();
        float3 wo = toLocal.rotate(view);
        // Construct individual lobes with their weights
        PrincipledBSDFLobes bsdf = PrincipledBSDFLobes(
            surface.baseColor, surface.metallic, surface.roughness,
            surface.ior, surface.transmission, true, wo);
        // Light sampling via Scene concrete-light MIS (NEE / Direct Lighting)
        // https://www.pbr-book.org/4ed/Light_Sources/Light_Sampling
        {
            LightSampleContext ctx = LightSampleContext(surface.hit.position, surface.hit.geometryNormal, surface.hit.normal);
            lastLightCtx = ctx;
            // SampleSceneLight returns: radiance = Li/(pdf*pChoice), pdf = mixture PDF
            LightLiSample Li = SampleSceneLight(ctx, env, rng.sample(), rng.sample2D(), totalLightWeight);
            SampledSpectrum radiance = Li.Li;
            // Shadow ray
            if (Li.pdf > EPS) {
                float3 shadowNormalFF = dot(Li.wi, surface.hit.geometryNormal) < 0.0f ? -surface.hit.geometryNormal : surface.hit.geometryNormal;
                float3 shadowOrigin = OffsetRay(surface.hit.position, shadowNormalFF);
                bool shadowed = TraceShadowRay(shadowOrigin, Li.wi, Li.dist);
                if (shadowed)
                    Li.pdf = 0.0f;
            }
            // Eval and add light contribution
            float3 wi = toLocal.rotate(Li.wi);
            float bsdfPdf = bsdf.PDF(wo, wi, TransportMode::Radiance, BxDFReflTransFlags::All);
            // MIS between light sampling path and BSDF sampling path
            if (Li.pdf > EPS) {
                float mis = Li.isDelta ? 1.0f : PowerHeuristic(1, Li.pdf, 1, bsdfPdf);
                if (bounce == 0) {
                    // First bounce: split direct lighting by lobe
                    float3 fD, fS;
                    bsdf.f_split(wo, wi, TransportMode::Radiance, fD, fS);
                    float3 scale = beta * AbsCosTheta(wi) * radiance * mis;
                    AddRadiance(LDiffuse, fD * scale, true);
                    AddRadiance(LSpecular, fS * scale, true);
                } else {
                    // Subsequent bounces: attribute all to the first-bounce category (indirect)
                    float3 f = bsdf.f(wo, wi, TransportMode::Radiance) * AbsCosTheta(wi);
                    float3 contrib = beta * f * radiance * mis;
                    if (firstBounceCategory == AOVCategory::Diffuse)
                        AddRadiance(LDiffuse, contrib, false);
                    else
                        AddRadiance(LSpecular, contrib, false);
                }
            }
        }
        // Emissive
        {
            float3 emContrib = beta * surface.emissive;
            if (bounce == 0) {
                // First-bounce emissive goes into diffuse
                AddRadiance(LDiffuse, emContrib, true);
            } else {
                if (firstBounceCategory == AOVCategory::Diffuse)
                    AddRadiance(LDiffuse, emContrib, false);
                else
                    AddRadiance(LSpecular, emContrib, false);
            }
        }
        // BSDF sampling (indirect lighting path)
        AOVCategory sampleCategory;
        BSDFSample sample = bsdf.Sample_f(wo, rng.sample(), rng.sample2D(),
                                          TransportMode::Radiance, BxDFReflTransFlags::All,
                                          sampleCategory);
        if (!sample.IsValid()) {
            break;
        }

        if (IsTransmission(sample.flags)) transmissionBounces++;
        else if (IsDiffuse(sample.flags)) diffuseBounces++;
        else specularBounces++;

        if (diffuseBounces > globalParams.ptMaxBouncesDiffuse ||
            specularBounces > globalParams.ptMaxBouncesSpecular ||
            transmissionBounces > globalParams.ptMaxBouncesTransmission) {
            break;
        }

        // AOV first bounce
        if (bounce == 0) {
            firstBounceCategory = sampleCategory;
        }
        lastSamplePDF = sample.pdf;
        beta *= sample.f * AbsCosTheta(sample.wi) / sample.pdf;
        if (any(isnan(beta)) || any(isinf(beta)))
            break;
        ray.Direction = toWorld.rotate(sample.wi);
        ray.Origin = OffsetRay(surface.hit.position, surface.hit.geometryNormal);
        // Russian Roulette (after 2nd bounce)
        if (bounce >= 2) {
            float rrPCount = min(MaxComponent(beta) + 0.01f, 0.95f);
            if (rng.sample() > rrPCount) // Contribution is low, may terminate early
                break;
            beta /= rrPCount; // Boost non-terminated paths
        }
    }
    // Write default GBuffer if no hit
    if (!gbufferWritten) {
        float4 g0 = float4(0, 0, 0, 0);
        float4 g1 = float4(0.5f, 0.5f, 1.0f, 0);  // neutral normal
        float4 g2 = float4(0, 0, 0, 0);
        if (frameCount == 1) {
            gBuffer0[pix] = g0;
            gBuffer1[pix] = g1;
            gBuffer2[pix] = g2;
        } else {
            float invN = 1.0f / frameCount;
            gBuffer0[pix] += (g0 - gBuffer0[pix]) * invN;
            gBuffer1[pix] += (g1 - gBuffer1[pix]) * invN;
            gBuffer2[pix] += (g2 - gBuffer2[pix]) * invN;
        }
        pickIDBuffer[pix] = ~0u; // no object
    }
    // Per-contribution sanitize already done via AddRadiance
    float4 outD = float4(LDiffuse, 1.0f);
    float4 outS = float4(LSpecular, 1.0f);

    // Welford online mean — buffer always holds the current average (immediate HDR)
    if (frameCount == 1) {
        diffuse[pix]  = outD;
        specular[pix] = outS;
    } else {
        float invN = 1.0f / frameCount;
        diffuse[pix]  += (outD - diffuse[pix])  * invN;
        specular[pix] += (outS - specular[pix]) * invN;
    }
}
// Naive stochastic opacity map implementation
// TODO: OMM? Maybe one day.
//  https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_opacity_micromap.html
//  https://devblogs.microsoft.com/directx/omm/
bool AnyHitOpacityIgnore<S : ISampler>(inout S rng, uint instance, uint primitive,
    BuiltInTriangleIntersectionAttributes attr)
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
    return false;
}

[shader("anyhit")] // Hit Group 0
void RayOpacityAnyHit(in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceIndex();
    uint primitive = PrimitiveIndex();
    PCG rng = PCG(DispatchRaysIndex().xy, globalParams.frameNumber, hash(uint2(instance, primitive)));
    if (AnyHitOpacityIgnore(rng, instance, primitive, attr))
        return IgnoreHit();
}

[shader("anyhit")] // Hit Group 1
void ShadowRayAnyHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceIndex();
    uint primitive = PrimitiveIndex();
    PCG rng = PCG(DispatchRaysIndex().xy, globalParams.frameNumber, hash(uint2(instance, primitive)));
    if (AnyHitOpacityIgnore(rng, instance, primitive, attr))
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