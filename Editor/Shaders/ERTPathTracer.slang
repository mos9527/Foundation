// References
// - PBRT v4
// - RAY TRACING GEMS II - Chapter 14
// - https://github.com/boksajak/referencePT/blob/master/shaders/PathTracer.hlsl
// - https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang
import IFlags;
import IMath;
import ICommon;
import IBRDF;
import ILight;
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
// -- glTF BSDF
// Energy compensation LUTs. See https://mos9527.com/posts/foundation/pt-6-path-tracing-adventures/#ee_avg-%E9%A2%84%E7%A7%AF%E5%88%86
[[vk_binding(0,1)]] SamplerState lutSampler;
[[vk_binding(1,1)]] Texture2D<float2> ggxLutE;
[[vk_binding(2,1)]] Texture2D<float> ggxLutEavg;
// Fresnel eval approximation of layered materials
// See https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#fresnel
public float3 glTFFresnelMix(float cosTheta, float3 bottom, float3 top, float ior = 1.5f){
    float F0 = pow((ior - 1) / (ior + 1), 2);
    float3 F = SchlickFresnel(F0, 1.0f, cosTheta);
    return lerp(bottom, top, F);
}
// Cheap, single-scattering LayeredBxDF alternative with glTF's metallic-roughness model
// See also Appendix B. https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#material-structure
public struct glTFBSDF : IBxDF {
    float3 baseColor;
    float metallic;
    float roughness;
    TrowbridgeReitzDistribution mfDistrib;

    bool energyCompensation;
    public __init(float3 baseColor, float metallic, float roughness, bool energyCompensation = true) {
        this.baseColor = baseColor;
        this.metallic = metallic;
        this.roughness = roughness;
        float alpha = roughness * roughness;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
        this.energyCompensation = energyCompensation;
    }
    public BxDFFlags Flags() {
        return BxDFFlags::Reflection | BxDFFlags::Glossy;
    }
    public float NEEGlossyProb(float3 wo) {
        // float F0 = pow((ior - 1) / (ior + 1), 2); // IOR=1.5->0.04
        float probGlossy = SchlickFresnel(0.04, 1.0f, ClampedCosTheta(wo));
        return lerp(probGlossy, 1.0f, metallic); // Fully metallic means there's no diffuse lobe
    }
    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode, BxDFReflTransFlags) {
        float c_min_reflectance = 0.04f;
        // Mixed Metallic/Dielectric Fresnel F0
        const float3 F0 = lerp(float3(c_min_reflectance), baseColor, metallic);
        const float3 F90 = 1.0f; // As per glTF spec
        // Multi-scatter compensation
        // https://mos9527.com/posts/foundation/pt-6-path-tracing-adventures/#%E8%83%BD%E9%87%8F%E5%AE%88%E6%81%92%E6%94%B9%E8%BF%9B
        const float mu = AbsCosTheta(wo);
        const float2 EEp = ggxLutE.SampleLevel(lutSampler, float2(mu, roughness), 0);
        const float3 Epp = F0 * EEp.x + (F90 - F0) * EEp.y;
        float probGlossy = NEEGlossyProb(wo);
        if (uc < probGlossy) {
            // Sample Glossy
            if (mfDistrib.EffectivelySmooth()) {
                // Dirac delta case
                float3 wi = float3(-wo.x, -wo.y, wo.z); // = wr
                wi = FaceForward(wi, float3(0,0,1));
                float3 wm = normalize(wi + wo);

                float3 Fss = SchlickFresnel(F0, 1.0f, AbsDot(wo, wm));
                float3 Fms = F0 * (1/Epp - 1) * Fss;
                if (!energyCompensation)
                    Fms = 0;
                float3 f = (Fss+Fms) / AbsCosTheta(wi);
                // Sampled PDF would be delta, but we represent them as 1s w/o weighting
                // With NEE this is what you get:
                float pdf = 1.0f * probGlossy;
                return BSDFSample(f, wi, pdf, BxDFFlags::SpecularReflection);
            } else {
                float3 wm = mfDistrib.Sample_wm(wo, u);
                float3 wi = Reflect(wo, wm);
                if (!SameHemisphere(wo, wi))
                    return BSDFSample(); // Absorption

                float3 Fss = SchlickFresnel(F0, 1.0f, AbsDot(wo, wm));
                float3 Fms = F0 * (1/Epp - 1) * Fss;
                if (!energyCompensation)
                    Fms = 0;
                float3 f = (Fms + Fss) * mfDistrib.D(wm) * mfDistrib.G(wo, wi) / (4 * AbsCosTheta(wi) * AbsCosTheta(wo));

                float pdf = probGlossy * mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm));
                return BSDFSample(f, wi, pdf, BxDFFlags::GlossyReflection);
            }
        } else {
            // Sample Diffuse
            float3 wi = SampleCosineHemisphere(u);
            wi = FaceForward(wi, float3(0,0,1));
            float3 wm = normalize(wo + wi);

            float3 cdiff = baseColor * (1.0f - metallic);
            float3 f = cdiff * InvPi;
            if (energyCompensation)
                f *= 1 - Epp;

            float pdf = (1 - probGlossy) * CosineHemispherePDF(ClampedCosTheta(wi));
            return BSDFSample(f, wi, pdf, BxDFFlags::DiffuseReflection);
        }
    }
    public SampledSpectrum f(float3 wo, float3 wi, TransportMode) {
        if (wo.z <= 0 || wi.z <= 0) return 0; // Only mirror reflection.
        float3 wm = normalize(wo + wi);
        float c_min_reflectance = 0.04f;
        float3 f0 = lerp(float3(c_min_reflectance), baseColor, metallic);
        // Same fresnel terms as before.
        float3 fGlossy = SchlickFresnel(f0, 1.0f, ClampedDot(wo, wm));
        float3 fDiffuse = (1.0f - SchlickFresnel(c_min_reflectance, 1.0f, AbsCosTheta(wo))) ;
        // The two lobes - remember NEE only affects the PDF in sampling, not the eval.
        SampledSpectrum diffuseBSDF = baseColor * fDiffuse * (1.0f - metallic) * InvPi;
        SampledSpectrum specularBSDF = mfDistrib.D(wm) * fGlossy * mfDistrib.G(wo, wi) / (4 * AbsCosTheta(wi) * AbsCosTheta(wo));
        // NEE for the combined lobe
        if (mfDistrib.EffectivelySmooth()){
            // PDF is dirac delta, which for eval is impossible to represent
            // This is the specular case again, so eval there has zero contribution
            return diffuseBSDF;
        } else {
            return diffuseBSDF + specularBSDF;
        }
    }
    public float PDF(float3 wo, float3 wi, TransportMode, BxDFReflTransFlags) {
        float3 wm = normalize(wo + wi);
        float diffusePDF = CosineHemispherePDF(ClampedCosTheta(wi));
        float specularPDF = mfDistrib.PDF(wo, wm) / (4 * ClampedDot(wo, wm));
        if (mfDistrib.EffectivelySmooth())
            specularPDF = 0.0f;
        return diffusePDF * (1 - NEEGlossyProb(wo)) + specularPDF * NEEGlossyProb(wo);
    }
};

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

    [mutating] void EnsureFrontFacing(float3 wo){
        if (dot(geometryNormal, wo) < 0.0f) geometryNormal *= -1;
        if (dot(geometryNormal, normal) < 0.0f) normal *= -1;
    }
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
    ray.TMax = 1e9;
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
    // Firefly reduction seen in nvpro-samples
    // TODO: "Next ray is at least as rough as previous ray" fixes it? - why?
    float maxRoughness = 0.0f;
    for (uint bounce = 0; bounce < globalParams.ptMaxBounces; bounce++){
        HitPayload payload;
        TraceRay(AS, 0, 0xFF, 0, 0, 0, ray, payload);
        // Outgoing direction
        float3 view = -ray.Direction;
        UniformInfiniteLight env = UniformInfiniteLight(globalParams.ambientColor, 1.0f);
        if (!payload.IsHit()){
            // Miss - environment
            // Since we're using MIS, ambient light is effectively a constant
            L += beta * env.Le(view);
            break;
        }
        Payload hit = SamplePayload(payload);
        hit.EnsureFrontFacing(view);
        // -- vv Debug View
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
        // -- ^^ Debug View
        /** Path Integrator **/
        // Local frame setup
        // vvv Investigate, supplied TBN frame shows lots of artifacts vvv
        quat toLocal     = quatRotateToZ(hit.normal);
        quat toWorld     = toLocal.inverted();
        float3 n = float3(0,0,1);
        float3 wo = toLocal.rotate(view);
        maxRoughness = max(maxRoughness, hit.roughness);
        IBxDF bsdf = glTFBSDF(hit.baseColor, hit.metallic, maxRoughness, true /* energy comp */);
        // IBxDF bsdf = DiffuseBxDF(hit.baseColor);
        // Direct Lighting
        // We only do one direct light sample for now.
        LightSampleContext ctx = LightSampleContext(hit.position, hit.geometryNormal, hit.normal);
        DistantLight sun = DistantLight(SampledSpectrum(globalParams.sunIntensity), globalParams.sunDirection);
        LightLiSample sunSample = sun.Sample_Li(ctx , rng.sample2D());
        if (AnyLuminance(sunSample.Li)) {
            // Sample sun direction
            float3 w = toLocal.rotate(sunSample.wi);
            float3 f = bsdf.f(wo, w) * ClampedCosTheta(w);
            // Shadow ray
            if (AnyLuminance(f)){
                float3 origin = OffsetRay(hit.position, hit.geometryNormal);
                bool occluded = TraceShadowRay(origin, sunSample.wi);
                if (!occluded)
                    L += beta * f * globalParams.sunIntensity;
            }
        }
        // Emissive
        L += beta * hit.emissive;
        BSDFSample sample = bsdf.Sample_f(wo, rng.sample(), rng.sample2D());
        if (!sample.IsValid())
            break;
        beta *= sample.f * AbsCosTheta(sample.wi) / sample.pdf;
        // Select next ray to sample from current position
        ray.Direction = toWorld.rotate(sample.wi);
        ray.Origin = OffsetRay(hit.position, hit.geometryNormal);
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
[shader("anyhit")] // Hit Group 0
void RayOpacityAnyHit(inout ShadowPayload payload)
{
    // TODO: Opacity map
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
[shader("anyhit")] // Hit Group 1
void ShadowRayAnyHit(inout ShadowPayload payload)
{
    payload.hit = true;
    AcceptHitAndEndSearch();
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
