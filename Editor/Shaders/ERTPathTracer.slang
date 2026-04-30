// References
// - PBRT v4
// - RAY TRACING GEMS II - Chapter 14
// - https://github.com/boksajak/referencePT/blob/master/shaders/PathTracer.hlsl
// - https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang
// And many more. Respective includes will tell.
import IFlags;
import ICommon;
import ILight;
import IBSDF;
import IBSSRDF;
import IMath;
import IBxDF;

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

static const uint kRayTypeSurface = 0u;
static const uint kRayTypeShadow = 1u;
static const uint kRayTypeBSSRDFQuery = 2u;
static const uint32_t kLightInstanceBit = 1u << 23;

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

uint GetMaterialBufferIndex(uint localMaterialIndex)
{
    return globalParams.firstMaterial + localMaterialIndex;
}

bool IsLightInstance(uint instance)
{
    return (instance & kLightInstanceBit) != 0u;
}

uint GetLightIndex(uint lightInstance)
{
    return lightInstance & ~kLightInstanceBit;
}

GSInstance LoadSceneInstance(uint localInstanceIndex)
{
    return instances[NonUniformResourceIndex(GetInstanceBufferIndex(localInstanceIndex))];
}

GSMaterial LoadMaterial(uint localMaterialIndex)
{
    return materials[NonUniformResourceIndex(GetMaterialBufferIndex(localMaterialIndex))];
}

GSLight LoadSceneLight(uint localLightIndex)
{
    return lights[NonUniformResourceIndex(globalParams.firstLight + localLightIndex)];
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
    // For normals, we need to multiply by inverse transpose of scale, which is 1.0 / scale
    float3 invScale = 1.0f / inst.scale;
    hit.normal         = normalize(inst.rotation.rotate(normal * invScale));
    hit.tangent        = normalize(inst.rotation.rotate(tangent * inst.scale));
    hit.geometryNormal = normalize(inst.rotation.rotate(geoNormal * invScale));
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

struct BSSRDFQueryPayload
{
    uint32_t targetInstance;
    uint32_t targetMaterial;
    WeightedReservoir<BSSRDFSurfaceCandidate> reservoir;
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
    float specularFactor;
    float subsurface;
    float3 subsurfaceColor;
    float3 subsurfaceRadius;
};

struct BSSRDFSurfaceSample
{
    bool valid;
    Payload surface;
    float3 Sp;
    float pdf;

    __init()
    {
        this.valid = false;
        this.surface = {};
        this.Sp = float3(0.0f);
        this.pdf = 0.0f;
    }
};

struct PathSample
{
    bool valid;
    float3 beta;
    float3 wi;
    float pdf;
    BxDFFlags flags;
    AOVCategory category;
    HitState hit;
    quat toWorld;

    __init()
    {
        this.valid = false;
        this.beta = float3(0.0f);
        this.wi = float3(0.0f);
        this.pdf = 0.0f;
        this.flags = BxDFFlags::Unset;
        this.category = AOVCategory::Diffuse;
        this.hit = {};
        this.toWorld = quat(float4(0, 0, 0, 1));
    }
};

struct PathRadiance
{
    float3 diffuse;
    float3 specular;

    __init()
    {
        this.diffuse = float3(0.0f);
        this.specular = float3(0.0f);
    }
};

struct DirectLightingState
{
    InfiniteLight env;
    float totalLightWeight;
    LightSampleContext lastLightCtx;

    __init(InfiniteLight env, float totalLightWeight)
    {
        this.env = env;
        this.totalLightWeight = totalLightWeight;
        this.lastLightCtx = LightSampleContext(float3(0), float3(0), float3(0));
    }
};

Payload SamplePayload(HitPayload payload, RayDesc ray)
{
    if (!payload.IsHit()) // Misuse
        return {};
    GSInstance inst = LoadSceneInstance(payload.instance);
    Payload res;
    res.materialID = inst.materialIndex;
    res.hit = getHitState(inst.meshOffset, payload.primitive, payload.bary, inst, ray.Origin, ray.Direction, payload.hitT);
    GSMaterial material = LoadMaterial(res.materialID);
    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    SampleMaterials(material, res.hit.uv, baseColor4, normalTBN, pbrMetalRough, emissive4);
    res.baseColor = baseColor4.xyz;
    res.emissive = emissive4.xyz;
    res.roughness = pbrMetalRough.y;
    res.metallic = pbrMetalRough.z;
    // Apply normal map in tangent space
    res.hit.normal = normalize(res.hit.tangent * normalTBN.x + res.hit.bitangent * normalTBN.y + res.hit.normal * normalTBN.z);
    res.ior = material.ior;
    res.specularFactor = material.specularFactor;
    res.transmission = material.transmissionFactor;
    res.subsurface = material.subsurfaceFactor;
    res.subsurfaceColor = material.subsurfaceColor;
    res.subsurfaceRadius = material.subsurfaceRadius;
    return res;
}

void AddRadiance(inout float3 L, float3 v, bool direct = false)
{
    if (any(isnan(v)) || any(isinf(v)))
        return;
    v = max(float3(0.0f), v);
    if (!direct){
        // Firefly clamp
        // Accounts for samples with very, very low PDFs, such as indirect emission that went through
        // a transmissive object (caustic refraction) or reflected off another specular surface (caustic reflection).
        // A low clamp value helps to reduce variance, albeit at the cost of accuracy (adding bias!)
        // Note that this is _not_ meant for _numerical errors_, which should not present themselves in the first place.
        float maxComp = MaxComponent(v);
        float clampEnergy = exp10(globalParams.ptFireflyClamp);
        if (maxComp > clampEnergy)
            v *= clampEnergy / maxComp;
    }
    L += v;
}

// Primary ray with jitter
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

float2 SampleAnamorphicLens(float2 u)
{
    uint blades = globalParams.apertureBlades;
    float2 bokeh = blades < 3u
        ? SampleConcentricDisk(u)
        : SampleRegularPolygonLens(float(blades), globalParams.apertureRotation, u);

    bokeh.x *= 1.0f / max(globalParams.apertureRatio, EPS);
    return bokeh;
}

RayDesc GenerateCameraRay<S : ISampler>(uint2 pixel, inout S rng)
{
    RayDesc ray;
    ray.Origin = globalParams.camPosition.xyz;
    ray.Direction = GeneratePrimaryRay(pixel, rng);
    ray.TMin = EPS_TMin;
    ray.TMax = INF;

    // Simple Physical Lens
    // https://www.pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models#fragment-Computepointonplaneoffocus-0
    // https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang#L922
    if (globalParams.aperture > EPS)
    {
        float3 focalPoint = globalParams.focalDistance * ray.Direction;
        float3 T = mul(globalParams.inverseView, float4(1, 0, 0, 0)).xyz;
        float3 B = mul(globalParams.inverseView, float4(0, 1, 0, 0)).xyz;
        float2 lensSample = SampleAnamorphicLens(rng.sample2D()) * globalParams.aperture;
        float3 lensPoint = lensSample.x * T + lensSample.y * B;
        ray.Origin += lensPoint;
        ray.Direction = normalize(focalPoint - lensPoint);
    }

    return ray;
}

// -- Main Shader Routines

// TraceRay via HitObject — returns a hit object without invoking closest hit.
// Used by SER to reorder threads before shading.
HitObject TraceHitObject(RayDesc ray)
{
    HitPayload payload;
    return HitObject.TraceRay(AS, 0, 0xFF, kRayTypeSurface, 0, 0, ray, payload);
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
    TraceRay(AS, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
             0x01, kRayTypeShadow, 1, 1, ray, payload);
    return payload.hit;
}

bool HasUsableBSSRDF(Payload surface)
{
    return surface.subsurface > EPS &&
           MaxComponent(surface.subsurfaceRadius) > EPS &&
           MaxComponent(surface.baseColor * surface.subsurfaceColor) > EPS;
}

BSSRDFSurfaceInteraction MakeBSSRDFPo(HitState hit)
{
    return BSSRDFSurfaceInteraction(hit.position, hit.normal, hit.tangent, hit.bitangent);
}

BSSRDFSurfaceInteraction MakeBSSRDFPi(HitState hit)
{
    return BSSRDFSurfaceInteraction(hit.position, hit.geometryNormal, hit.tangent, hit.bitangent);
}

bool SeparableBSSRDF_SampleSp<S : ISampler>(
    Payload surface,
    uint32_t sourceInstance,
    inout S rng,
    out BSSRDFSurfaceSample result)
{
    result = BSSRDFSurfaceSample();
    if (!HasUsableBSSRDF(surface))
        return false;

    ISeparableBSSRDF bssrdf = DisneyBurleyBSSRDF(surface.baseColor * surface.subsurfaceColor, surface.subsurfaceRadius);
    BSSRDFSurfaceInteraction poBSSRDF = MakeBSSRDFPo(surface.hit);
    BSSRDFSampleRaySegment segment =
        bssrdf.Sample_SpRay(poBSSRDF, rng.sample(), rng.sample(), rng.sample(), rng.sample());
    if (!segment.valid)
        return false;

    RayDesc queryRay;
    queryRay.Origin = segment.origin;
    queryRay.Direction = segment.direction;
    queryRay.TMin = EPS_TMin;
    queryRay.TMax = segment.tMax;

    BSSRDFQueryPayload queryPayload;
    queryPayload.targetInstance = sourceInstance;
    queryPayload.targetMaterial = surface.materialID;
    queryPayload.reservoir = WeightedReservoir<BSSRDFSurfaceCandidate>();
    TraceRay(AS, 0, 0x01, kRayTypeBSSRDFQuery, 1, 2, queryRay, queryPayload);

    if (!queryPayload.reservoir.HasSample())
        return false;

    BSSRDFSurfaceCandidate selected = queryPayload.reservoir.selected;
    HitPayload selectedHit;
    selectedHit.instance = selected.instance;
    selectedHit.primitive = selected.primitive;
    selectedHit.bary = selected.bary;
    selectedHit.hitT = selected.hitT;

    Payload pi = SamplePayload(selectedHit, queryRay);
    float selectionPdf = queryPayload.reservoir.SelectionPDF();
    BSSRDFSurfaceInteraction piBSSRDF = MakeBSSRDFPi(pi.hit);
    float pdfSp = bssrdf.Pdf_Sp(poBSSRDF, piBSSRDF) * selectionPdf;
    float3 Sp = bssrdf.Sp(poBSSRDF, piBSSRDF);
    if (pdfSp <= EPS || MaxComponent(Sp) <= EPS)
        return false;

    result.valid = true;
    result.surface = pi;
    result.Sp = Sp;
    result.pdf = pdfSp;
    return true;
}

float GetEnvLightSelectionWeight()
{
    return globalParams.useEnvMap != 0u ? globalParams.envMapScale : globalParams.ambientPower;
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
        GSLight gl = LoadSceneLight(idx);
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

void AddRadianceByCategory(
    inout PathRadiance radiance,
    float3 contrib,
    uint bounce,
    AOVCategory firstBounceCategory,
    AOVCategory currentCategory,
    bool direct)
{
    AOVCategory category = bounce == 0 ? currentCategory : firstBounceCategory;
    if (category == AOVCategory::Diffuse)
        AddRadiance(radiance.diffuse, contrib, direct);
    else
        AddRadiance(radiance.specular, contrib, direct);
}

bool SampleVisibleSceneLight<S : ISampler>(
    HitState hit,
    InfiniteLight env,
    inout S rng,
    float totalLightWeight,
    out LightSampleContext ctx,
    out LightLiSample Li)
{
    ctx = LightSampleContext(hit.position, hit.geometryNormal, hit.normal);
    Li = SampleSceneLight(ctx, env, rng.sample(), rng.sample2D(), totalLightWeight);
    if (Li.pdf <= EPS)
        return false;

    float3 shadowNormalFF = dot(Li.wi, hit.geometryNormal) < 0.0f ? -hit.geometryNormal : hit.geometryNormal;
    float3 shadowOrigin = OffsetRay(hit.position, shadowNormalFF);
    if (TraceShadowRay(shadowOrigin, Li.wi, Li.dist))
        Li.pdf = 0.0f;

    return Li.pdf > EPS;
}

void AddLocalDirectLighting<S : ISampler>(
    Payload surface,
    PrincipledBSDFLobes bsdf,
    float3 wo,
    quat toLocal,
    float3 beta,
    uint bounce,
    AOVCategory firstBounceCategory,
    inout S rng,
    inout DirectLightingState lighting,
    inout PathRadiance radiance)
{
    LightSampleContext ctx;
    LightLiSample Li;
    if (!SampleVisibleSceneLight(surface.hit, lighting.env, rng, lighting.totalLightWeight, ctx, Li))
    {
        lighting.lastLightCtx = ctx;
        return;
    }

    lighting.lastLightCtx = ctx;
    float3 wi = toLocal.rotate(Li.wi);
    float bsdfPdf = bsdf.PDF(wo, wi, TransportMode::Radiance, BxDFReflTransFlags::All);
    float mis = Li.isDelta ? 1.0f : PowerHeuristic(1, Li.pdf, 1, bsdfPdf);

    if (bounce == 0)
    {
        float3 fD, fS;
        bsdf.f_split(wo, wi, TransportMode::Radiance, fD, fS);
        float3 scale = beta * AbsCosTheta(wi) * Li.Li * mis;
        AddRadiance(radiance.diffuse, fD * scale, true);
        AddRadiance(radiance.specular, fS * scale, true);
    }
    else
    {
        float3 f = bsdf.f(wo, wi, TransportMode::Radiance) * AbsCosTheta(wi);
        AddRadianceByCategory(radiance, beta * f * Li.Li * mis,
                              bounce, firstBounceCategory, AOVCategory::Diffuse, false);
    }
}

void AddDiffuseDirectLighting<S : ISampler>(
    HitState hit,
    quat toLocal,
    float3 f,
    float3 beta,
    uint bounce,
    AOVCategory firstBounceCategory,
    AOVCategory currentCategory,
    inout S rng,
    inout DirectLightingState lighting,
    inout PathRadiance radiance)
{
    LightSampleContext ctx;
    LightLiSample Li;
    if (!SampleVisibleSceneLight(hit, lighting.env, rng, lighting.totalLightWeight, ctx, Li))
    {
        lighting.lastLightCtx = ctx;
        return;
    }

    lighting.lastLightCtx = ctx;
    float3 wi = toLocal.rotate(Li.wi);
    float diffusePdf = CosineHemispherePDF(AbsCosTheta(wi));
    float mis = Li.isDelta ? 1.0f : PowerHeuristic(1, Li.pdf, 1, diffusePdf);
    float3 contrib = beta * f * AbsCosTheta(wi) * Li.Li * mis;
    AddRadianceByCategory(radiance, contrib, bounce, firstBounceCategory, currentCategory, true);
}

void AddSurfaceEmissive(
    Payload surface,
    float3 beta,
    uint bounce,
    AOVCategory firstBounceCategory,
    inout PathRadiance radiance)
{
    AddRadianceByCategory(radiance, beta * surface.emissive,
                          bounce, firstBounceCategory, AOVCategory::Diffuse, bounce == 0);
}

void AccumulateGBuffer(uint2 pix, float4 g0, float4 g1, float4 g2, uint32_t pickID, float frameCount)
{
    if (frameCount == 1)
    {
        gBuffer0[pix] = g0;
        gBuffer1[pix] = g1;
        gBuffer2[pix] = g2;
    }
    else
    {
        float invN = 1.0f / frameCount;
        gBuffer0[pix] += (g0 - gBuffer0[pix]) * invN;
        gBuffer1[pix] += (g1 - gBuffer1[pix]) * invN;
        gBuffer2[pix] += (g2 - gBuffer2[pix]) * invN;
    }

    pickIDBuffer[pix] = pickID;
}

void AccumulateSurfaceGBuffer(uint2 pix, Payload surface, uint32_t instance, float frameCount)
{
    float4 g0 = float4(surface.baseColor, float(surface.materialID) / 255.0f);
    float4 g1 = float4(surface.hit.normal * 0.5f + 0.5f, 0);
    float4 g2 = float4(surface.hit.position, 0);
    AccumulateGBuffer(pix, g0, g1, g2, instance, frameCount);
}

void AccumulateDefaultGBuffer(uint2 pix, float frameCount)
{
    float4 g0 = float4(0, 0, 0, 0);
    float4 g1 = float4(0.5f, 0.5f, 1.0f, 0);
    float4 g2 = float4(0, 0, 0, 0);
    AccumulateGBuffer(pix, g0, g1, g2, ~0u, frameCount);
}

bool AddAreaLightHitEmission(
    HitPayload payload,
    RayDesc ray,
    float3 view,
    float3 beta,
    uint bounce,
    AOVCategory firstBounceCategory,
    float lastSamplePDF,
    DirectLightingState lighting,
    inout PathRadiance radiance,
    out float3 hitPoint)
{
    hitPoint = float3(0.0f);
    if (!payload.IsHit() || !IsLightInstance(payload.instance))
        return false;

    uint lightIndex = GetLightIndex(payload.instance);
    GSLight gl = LoadSceneLight(lightIndex);
    hitPoint = ray.Origin + ray.Direction * payload.hitT;

    SampledSpectrum Lemit = SampledSpectrum(gl.color * gl.power);
    SampledSpectrum Le = SampledSpectrum(0.0f);
    float pdfLi = 0.0f;

    if (gl.type == 3)
    {
        DiskLight light = DiskLight(Lemit, gl.position, gl.dpdu, gl.dpdv, gl.radius, gl.twoSided != 0);
        float3 normal = normalize(cross(gl.dpdu, gl.dpdv));
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, ray.Direction, true);
    }
    else if (gl.type == 4)
    {
        RectLight light = RectLight(Lemit, gl.position, gl.dpdu, gl.dpdv, gl.twoSided != 0);
        float3 normal = normalize(cross(gl.dpdu, gl.dpdv));
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, ray.Direction, true);
    }

    if (any(Le > 0.0f))
    {
        float pLightSelection = lighting.totalLightWeight > 0.0f
            ? (gl.selectionWeight / lighting.totalLightWeight)
            : 0.0f;
        float pdf = pLightSelection * pdfLi;
        float mis = lastSamplePDF > EPS ? BalanceHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
        float3 contrib = beta * Le * mis;
        if (bounce == 0)
            AddRadiance(radiance.specular, contrib, true);
        else
            AddRadianceByCategory(radiance, contrib, bounce, firstBounceCategory, AOVCategory::Specular, false);
    }

    return true;
}

void AddEnvironmentMiss(
    float3 view,
    float3 beta,
    uint bounce,
    AOVCategory firstBounceCategory,
    float lastSamplePDF,
    DirectLightingState lighting,
    inout PathRadiance radiance)
{
    if (bounce == 0)
    {
        AddRadiance(radiance.specular, lighting.env.Le(-view), true);
        return;
    }

    float envWeight = GetEnvLightSelectionWeight();
    float pdf = lighting.totalLightWeight > 0.0f
        ? (envWeight / lighting.totalLightWeight) * lighting.env.PDF_Li(lighting.lastLightCtx, -view, true)
        : 0.0f;
    float mis = lastSamplePDF > EPS ? PowerHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
    float3 contrib = beta * lighting.env.Le(-view) * mis;
    AddRadianceByCategory(radiance, contrib, bounce, firstBounceCategory, AOVCategory::Specular, false);
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
    PathRadiance radiance = PathRadiance();
    AOVCategory firstBounceCategory = AOVCategory::Undefined;
    bool gbufferWritten = false;
    float frameCount = float(globalParams.ptAccumulatedFrames + 1);
    RayDesc ray = GenerateCameraRay(pix, rng);
    // Combine the miss hit with the last light sample
    float lastSamplePDF = 0.0f;
    // Unified infinite light — falls back to constant color when no env map is bound
    InfiniteLight env = InfiniteLight(
        SampledSpectrum(globalParams.ambientColor * globalParams.ambientPower),
        globalParams.envMapScale,
        globalParams.useEnvMap != 0u,
        globalParams.envAzimuthOffset
    );
    float totalLightWeight = globalParams.sceneLightWeightSum + GetEnvLightSelectionWeight();
    DirectLightingState lighting = DirectLightingState(env, totalLightWeight);
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
                uint instanceID = hitObj.GetInstanceID();
                if (!IsLightInstance(instanceID)) {
                    GSInstance inst = LoadSceneInstance(instanceID);
                    coherenceHint = inst.materialIndex & 0xFFu;
                }
            }
            ReorderThread(hitObj, coherenceHint, kSERCoherenceHintBits);
            // Invoke closest hit / miss to populate the payload
            HitObject::Invoke(AS, hitObj, payload);
        } else
            TraceRay(AS, 0, 0xFF, kRayTypeSurface, 0, 0, ray, payload);
        // Outgoing direction
        float3 view = -ray.Direction;

        float3 hitPoint;
        if (AddAreaLightHitEmission(payload, ray, view, beta, bounce, firstBounceCategory,
                                    lastSamplePDF, lighting, radiance, hitPoint))
        {
            ray.Origin = hitPoint + ray.Direction * EPS_TMin;
            bounce--; // Don't count this as a bounce
            continue;
        }

        if (!payload.IsHit())
        {
            AddEnvironmentMiss(view, beta, bounce, firstBounceCategory, lastSamplePDF, lighting, radiance);
            break;
        }
        Payload surface = SamplePayload(payload, ray);

        // Accumulate GBuffer on primary hit
        if (bounce == 0 && !gbufferWritten)
        {
            AccumulateSurfaceGBuffer(pix, surface, payload.instance, frameCount);
            gbufferWritten = true;
        }
        // -- Path Integrator
        // Local frame setup
        quat toLocal = quatRotateToZ(surface.hit.normal);
        quat toWorld = toLocal.inverted();
        float3 wo = toLocal.rotate(view);
        // Construct local lobes with their weights
        PrincipledBSDFLobes bsdf = PrincipledBSDFLobes(
            surface.baseColor, surface.metallic, surface.roughness,
            surface.ior, surface.specularFactor, surface.transmission, true, wo,
            surface.subsurface);
        // Subsurface lobes are non-local. We take care of those paths ab initio with a different branch
        float pSampleBSSRDF = HasUsableBSSRDF(surface)
            ? PrincipledBSSRDFBranchProbability(surface.metallic, surface.roughness, surface.ior,
                                                surface.specularFactor, surface.transmission, surface.subsurface, wo)
            : 0.0f;
        pSampleBSSRDF = clamp(pSampleBSSRDF, 0.0f, 1.0f);
        bool sampleBSSRDF = pSampleBSSRDF > EPS && (pSampleBSSRDF >= 1.0f || rng.sample() < pSampleBSSRDF);
        // -- Path Trace
        PathSample nextSample = PathSample();
        // TODO Volumetrics
        if (sampleBSSRDF)
        {
            BSSRDFSurfaceSample sample;
            if (!SeparableBSSRDF_SampleSp(surface, payload.instance, rng, sample))
                break;

            float3 localBeta = beta * sample.Sp / max(pSampleBSSRDF * sample.pdf, EPS);
            if (any(isnan(localBeta)) || any(isinf(localBeta)))
                break;

            Payload exitSurface = sample.surface;
            quat exitToLocal = quatRotateToZ(exitSurface.hit.normal);
            quat exitToWorld = exitToLocal.inverted();
            // Lambertian assumed for subsurface interactions
            const float3 exitF = float3(InvPi);
            AddDiffuseDirectLighting(exitSurface.hit, exitToLocal, exitF, localBeta,
                                     bounce, firstBounceCategory, AOVCategory::Diffuse,
                                     rng, lighting, radiance);

            float3 exitWi = SampleCosineHemisphere(rng.sample2D());
            exitWi = FaceForward(exitWi, float3(0, 0, 1));
            float exitPdf = CosineHemispherePDF(AbsCosTheta(exitWi));
            if (exitPdf <= EPS)
                break;

            nextSample.valid = true;
            nextSample.beta = localBeta * exitF * AbsCosTheta(exitWi) / exitPdf;
            nextSample.wi = exitWi;
            nextSample.pdf = exitPdf;
            nextSample.flags = BxDFFlags::DiffuseReflection;
            nextSample.category = AOVCategory::Diffuse;
            nextSample.hit = exitSurface.hit;
            nextSample.toWorld = exitToWorld;
        }
        else
        {
            float pLocal = max(1.0f - pSampleBSSRDF, EPS);
            float3 localBeta = beta / pLocal;

            AddLocalDirectLighting(surface, bsdf, wo, toLocal, localBeta,
                                   bounce, firstBounceCategory, rng, lighting, radiance);
            AddSurfaceEmissive(surface, localBeta, bounce, firstBounceCategory, radiance);

            AOVCategory sampleCategory;
            BSDFSample sample = bsdf.Sample_f(wo, rng.sample(), rng.sample2D(),
                                              TransportMode::Radiance, BxDFReflTransFlags::All,
                                              sampleCategory);
            if (!sample.IsValid()) {
                break;
            }

            nextSample.valid = true;
            nextSample.beta = localBeta * sample.f * AbsCosTheta(sample.wi) / sample.pdf;
            nextSample.wi = sample.wi;
            nextSample.pdf = sample.pdf;
            nextSample.flags = sample.flags;
            nextSample.category = sampleCategory;
            nextSample.hit = surface.hit;
            nextSample.toWorld = toWorld;
        }

        if (!nextSample.valid)
            break;

        if (IsTransmission(nextSample.flags)) transmissionBounces++;
        else if (IsDiffuse(nextSample.flags)) diffuseBounces++;
        else specularBounces++;

        if (diffuseBounces > globalParams.ptMaxBouncesDiffuse ||
            specularBounces > globalParams.ptMaxBouncesSpecular ||
            transmissionBounces > globalParams.ptMaxBouncesTransmission) {
            break;
        }

        if (bounce == 0)
            firstBounceCategory = nextSample.category;

        lastSamplePDF = nextSample.pdf;
        beta = nextSample.beta;
        if (any(isnan(beta)) || any(isinf(beta)))
            break;

        ray.Direction = nextSample.toWorld.rotate(nextSample.wi);
        float3 rayNormalFF = dot(ray.Direction, nextSample.hit.geometryNormal) < 0.0f ? -nextSample.hit.geometryNormal : nextSample.hit.geometryNormal;
        ray.Origin = OffsetRay(nextSample.hit.position, rayNormalFF);

        // Russian Roulette (after 2nd bounce)
        if (bounce >= 2) {
            float rrPCount = min(MaxComponent(beta) + 0.01f, 0.95f);
            if (rng.sample() > rrPCount) // Contribution is low, may terminate early
                break;
            beta /= rrPCount; // Boost non-terminated paths
        }
    }
    // Write default GBuffer if no hit
    if (!gbufferWritten)
        AccumulateDefaultGBuffer(pix, frameCount);

    // Per-contribution sanitize already done via AddRadiance
    float4 outD = float4(radiance.diffuse, 1.0f);
    float4 outS = float4(radiance.specular, 1.0f);

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
    if (IsLightInstance(instance))
        return false; // Light instance

    GSInstance inst = LoadSceneInstance(instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);
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
    uint instance = InstanceID();
    uint primitive = PrimitiveIndex();
    PCG rng = PCG(DispatchRaysIndex().xy, globalParams.frameNumber, hash(uint2(instance, primitive)));
    if (AnyHitOpacityIgnore(rng, instance, primitive, attr))
        return IgnoreHit();
}

[shader("anyhit")] // Hit Group 1
void ShadowRayAnyHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceID();
    uint primitive = PrimitiveIndex();
    PCG rng = PCG(DispatchRaysIndex().xy, globalParams.frameNumber, hash(uint2(instance, primitive)));
    if (AnyHitOpacityIgnore(rng, instance, primitive, attr))
        return IgnoreHit();
    payload.hit = true;
    AcceptHitAndEndSearch();
}

[shader("anyhit")] // Hit Group 2
void BSSRDFQueryAnyHit(inout BSSRDFQueryPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceID();
    uint primitive = PrimitiveIndex();
    if (IsLightInstance(instance))
        return IgnoreHit();

    PCG rng = PCG(DispatchRaysIndex().xy, globalParams.frameNumber,
                  hash(uint3(instance, primitive, payload.reservoir.sampleCount)));
    if (AnyHitOpacityIgnore(rng, instance, primitive, attr))
        return IgnoreHit();

    GSInstance inst = LoadSceneInstance(instance);
    if (instance == payload.targetInstance && inst.materialIndex == payload.targetMaterial)
    {
        BSSRDFSurfaceCandidate candidate =
            BSSRDFSurfaceCandidate(instance, primitive, attr.barycentrics.xy, RayTCurrent());
        payload.reservoir.Add(candidate, 1.0f, rng.sample());
    }

    return IgnoreHit();
}

[shader("closesthit")] // Hit Group 0
void RayClosestHit(inout HitPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    payload.instance = InstanceID();
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

[shader("miss")] // Miss 2
void BSSRDFQueryMiss(inout BSSRDFQueryPayload payload)
{
}