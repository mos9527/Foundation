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
import IMaterial;
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
StructuredBuffer<GSCurveInstance> curveInstances;
ByteAddressBuffer primitives;
StructuredBuffer<GSMaterial> materials;
StructuredBuffer<GSLight> lights;
StructuredBuffer<Alias> lightAliasTable;
SamplerState textureSampler;
StructuredBuffer<uint> sobolMatrices;
// -- Path radiance accumulation buffers (Welford online mean)
RWTexture2D<float4> diffuse;   // Diffuse lobe radiance
RWTexture2D<float4> specular;  // Specular lobe radiance (incl. transmission)
RWTexture2D<uint32_t> pickIDBuffer; // Instance ID map, ~0u = no object (written on primary hit only)
[[vk_binding(0,2)]] Texture2D<float4> textures[];

// SER coherence hint layout:
//   [7:6] hit class (miss, analytic light, triangle, curve)
//   [5:0] mixed material/light key
static const uint kSERCoherenceHintBits = 8u;
// Packed path-tracer compile options. Must match Render.hpp.
static const uint kPTCompileOptionShaderExecutionReordering = 1u << 0;
static const uint kPTCompileOptionSamplerSobol = 1u << 1;
static const uint kPTCompileOptionSamplerPCG = 1u << 2;
[[SpecializationConstant]] uint ptCompileOptions;

static const uint kRayTypeSurface = 0u;
static const uint kRayTypeShadow = 1u;
static const uint kRayTypeBSSRDFQuery = 2u;
static const uint32_t kLightInstanceBit = 1u << 23;
static const uint32_t kCurveInstanceBit = 1u << 22;
static const uint kMeshMask = 0x01;
static const uint kCurveMask = 0x04;

// Tiling helpers
uint GetPTTileSide()
{
    return max(globalParams.ptDispatchTileSide, 1u);
}

uint GetPTSamplesPerDispatch()
{
    return max(globalParams.ptSamplesPerPixel, 1u);
}

uint GetPTTileSampleCount()
{
    uint tileSide = GetPTTileSide();
    return max(tileSide * tileSide, 1u);
}

uint GetPTBasePixelSampleCount()
{
    return globalParams.ptAccumulatedFrames / GetPTTileSampleCount();
}

uint2 GetPTPixel()
{
    uint tileSide = GetPTTileSide();
    uint sampleIndex = globalParams.ptAccumulatedFrames % GetPTTileSampleCount();
    uint2 sampleOffset = uint2(sampleIndex % tileSide, sampleIndex / tileSide);
    return DispatchRaysIndex().xy * tileSide + sampleOffset;
}

bool IsPTPixelInBounds(uint2 pix)
{
    return pix.x < uint(globalParams.fbWidth) && pix.y < uint(globalParams.fbHeight);
}

void ClearPTTile()
{
    uint tileSide = GetPTTileSide();
    if (tileSide <= 1u || globalParams.ptAccumulatedFrames != 0u)
        return;

    uint2 tileOrigin = DispatchRaysIndex().xy * tileSide;
    uint2 extent = uint2(uint(globalParams.fbWidth), uint(globalParams.fbHeight));
    for (uint y = 0u; y < tileSide; y++)
    {
        for (uint x = 0u; x < tileSide; x++)
        {
            uint2 clearPix = tileOrigin + uint2(x, y);
            if (any(clearPix >= extent))
                continue;

            diffuse[clearPix] = float4(0);
            specular[clearPix] = float4(0);
            pickIDBuffer[clearPix] = ~0u;
        }
    }
}

uint GetPTPixelSampleCount(uint subSample)
{
    return GetPTBasePixelSampleCount() + subSample + 1u;
}

// Hit state helpers
struct HitState
{
    float3 position;
    float3 normal;
    float3 geometryNormal;
    float3 tangent;
    float3 bitangent;
    float2 uv;
};

float3 WorldToShadingLocal(HitState hit, float3 v)
{
    return float3(dot(v, hit.tangent), dot(v, hit.bitangent), dot(v, hit.normal));
}

float3 ShadingLocalToWorld(HitState hit, float3 v)
{
    return v.x * hit.tangent + v.y * hit.bitangent + v.z * hit.normal;
}

void SetHitShadingNormal(inout HitState hit, float3 normal)
{
    float bitangentSign = dot(cross(hit.normal, hit.tangent), hit.bitangent) >= 0.0f ? 1.0f : -1.0f;
    if (LengthSquared(normal) <= 1e-8f)
        normal = hit.normal;
    hit.normal = normalize(normal);

    float3 tangent = hit.tangent - hit.normal * dot(hit.tangent, hit.normal);
    if (LengthSquared(tangent) <= 1e-8f)
    {
        float3 fallbackBitangent;
        buildOrthonormalBasis(hit.normal, tangent, fallbackBitangent);
        hit.tangent = normalize(tangent);
        hit.bitangent = normalize(fallbackBitangent) * bitangentSign;
        return;
    }

    hit.tangent = normalize(tangent);
    hit.bitangent = normalize(cross(hit.normal, hit.tangent)) * bitangentSign;
}

void RotateHitTangentFrame(inout HitState hit, float2 direction)
{
    if (LengthSquared(direction) <= 1e-8f)
        direction = float2(1.0f, 0.0f);
    direction = normalize(direction);

    float3 tangent = hit.tangent;
    float3 bitangent = hit.bitangent;
    hit.tangent = normalize(direction.x * tangent + direction.y * bitangent);
    hit.bitangent = normalize(-direction.y * tangent + direction.x * bitangent);
}

uint GetInstanceBufferIndex(uint localInstanceIndex)
{
    return globalParams.firstInstance + localInstanceIndex;
}

uint GetCurveInstanceBufferIndex(uint localCurveInstanceIndex)
{
    return globalParams.firstCurveInstance + localCurveInstanceIndex;
}

uint GetMaterialBufferIndex(uint localMaterialIndex)
{
    return globalParams.firstMaterial + localMaterialIndex;
}

bool IsLightInstance(uint instance)
{
    return (instance & kLightInstanceBit) != 0u;
}

bool IsCurveInstance(uint instance)
{
    return (instance & kCurveInstanceBit) != 0u;
}

uint GetLightIndex(uint lightInstance)
{
    return lightInstance & ~kLightInstanceBit;
}

uint GetCurveIndex(uint curveInstance)
{
    return curveInstance & ~kCurveInstanceBit;
}

GSInstance LoadSceneInstance(uint localInstanceIndex)
{
    return instances[NonUniformResourceIndex(GetInstanceBufferIndex(localInstanceIndex))];
}

GSCurveInstance LoadSceneCurveInstance(uint localCurveInstanceIndex)
{
    return curveInstances[NonUniformResourceIndex(GetCurveInstanceBufferIndex(localCurveInstanceIndex))];
}

GSMaterial LoadMaterial(uint localMaterialIndex)
{
    return materials[NonUniformResourceIndex(GetMaterialBufferIndex(localMaterialIndex))];
}

GSLight LoadSceneLight(uint localLightIndex)
{
    return lights[NonUniformResourceIndex(globalParams.firstLight + localLightIndex)];
}

HitState GetHitState(uint meshOffset, uint primitive, float2 bary, GSInstance inst, float3 rayOrigin, float3 rayDirection, float hitT)
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

HitState GetCurveHitState(uint curveOffset, uint primitive, float2 uv, GSCurveInstance inst,
                          float3 rayOrigin, float3 rayDirection, float hitT)
{
    HitState hit;
    hit.position = rayOrigin + rayDirection * hitT;
    hit.uv = uv;

    GSCurveSet curve = primitives.Load<GSCurveSet>(curveOffset);
    GSCurveSegment segment = primitives.Load<GSCurveSegment>(curve.segmentOffset + primitive * sizeof(GSCurveSegment));
    GSCurvePoint p0 = primitives.Load<GSCurvePoint>(curve.pointOffset + segment.p0 * sizeof(GSCurvePoint));
    GSCurvePoint p1 = primitives.Load<GSCurvePoint>(curve.pointOffset + segment.p1 * sizeof(GSCurvePoint));

    float3 localPosition = inst.rotation.conjugate().rotate(hit.position - inst.transform) / inst.scale;
    float3 axis = p1.position - p0.position;
    float axisLen2 = max(dot(axis, axis), 1e-12f);
    float u = saturate(dot(localPosition - p0.position, axis) / axisLen2);
    float3 closest = lerp(p0.position, p1.position, u);
    float3 tangent = normalize(axis);
    float3 radial = localPosition - closest;
    radial = LengthSquared(radial) > 1e-12f ? normalize(radial) : normalize(cross(tangent, float3(0.0f, 1.0f, 0.0f)));
    float radiusSlope = (p1.radius - p0.radius) * rsqrt(axisLen2);
    float3 localNormal = normalize(radial - tangent * radiusSlope);
    if (any(isnan(localNormal)))
        localNormal = float3(1.0f, 0.0f, 0.0f);

    float3 invScale = 1.0f / inst.scale;
    hit.normal = normalize(inst.rotation.rotate(localNormal * invScale));
    hit.geometryNormal = hit.normal;
    hit.tangent = normalize(inst.rotation.rotate(tangent * inst.scale));
    hit.bitangent = normalize(cross(hit.normal, hit.tangent));
    return hit;
}

static const uint32_t INSTANCE_NO_HIT = ~0u;

enum HitKind
{
    None = 0,
    Triangle = 1,
    Disk = 2,
    Rect = 3,
    Curve = 4
};

struct HitPayload
{
    uint32_t instance;
    uint32_t primitive;
    float2 bary;
    float hitT;

    bool IsHit() { return instance != INSTANCE_NO_HIT; }
};

struct HitRecord
{
    HitKind kind;
    HitPayload raw;
    uint32_t lightIndex;
    float3 position;

    __init()
    {
        this.kind = HitKind::None;
        this.raw = {};
        this.lightIndex = ~0u;
        this.position = float3(0.0f);
    }
};

HitKind HitKindFromLight(GSLight light)
{
    if (light.type == 3)
        return HitKind::Disk;
    if (light.type == 4)
        return HitKind::Rect;
    return HitKind::None;
}

bool IsAnalyticLightHit(HitKind kind)
{
    return kind == HitKind::Disk || kind == HitKind::Rect;
}

bool IsMaterialSurfaceHit(HitKind kind)
{
    return kind == HitKind::Triangle || kind == HitKind::Curve;
}

HitRecord MakeHitRecord(HitPayload payload, RayDesc ray)
{
    HitRecord hit = HitRecord();
    hit.raw = payload;
    if (!payload.IsHit())
        return hit;

    hit.position = ray.Origin + ray.Direction * payload.hitT;
    if (IsLightInstance(payload.instance))
    {
        hit.lightIndex = GetLightIndex(payload.instance);
        hit.kind = HitKindFromLight(LoadSceneLight(hit.lightIndex));
        return hit;
    }
    if (IsCurveInstance(payload.instance))
    {
        hit.kind = HitKind::Curve;
        return hit;
    }

    hit.kind = HitKind::Triangle;
    return hit;
}

// Hit Payload helpers
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
    float specularFactor;
    float3 specularColorFactor;
    float anisotropyStrength;
    float subsurface;
    float subsurfaceScale;
    float3 subsurfaceColor;
    float3 subsurfaceRadius;
};

struct BSSRDFQueryPayload
{
    uint32_t targetInstance;
    uint32_t targetMaterial;
    WeightedReservoir<BSSRDFSurfaceCandidate> reservoir;
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

    __init()
    {
        this.valid = false;
        this.beta = float3(0.0f);
        this.wi = float3(0.0f);
        this.pdf = 0.0f;
        this.flags = BxDFFlags::Unset;
        this.category = AOVCategory::Diffuse;
        this.hit = {};
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
    float lightTotalWeight;
    LightSampleContext lastLightCtx;

    __init(InfiniteLight env, float lightTotalWeight)
    {
        this.env = env;
        this.lightTotalWeight = lightTotalWeight;
        this.lastLightCtx = LightSampleContext(float3(0), float3(0), float3(0));
    }
};

// Material helpers
// vvvvv TODO More materials? Custom ones?
//       One uber shader here is surely insufficent. Need to compile ad-hoc shaders _here_.
IMaterial MakeMaterial(Payload surface, float3 wo)
{
    return PrincipledMaterial(
        surface.baseColor, surface.metallic, surface.roughness,
        surface.ior, surface.specularFactor, surface.transmission, true, wo,
        surface.subsurface, surface.subsurfaceColor, surface.subsurfaceRadius, surface.subsurfaceScale,
        surface.anisotropyStrength, surface.specularColorFactor);
}


void SampleMaterials(GSMaterial material, float2 uv,
    out float4 baseColor, out float4 normalTBN, out float4 pbrMetalRough, out float4 emissive,
    out float transmission, out float specularFactor, out float3 specularColorFactor,
    out float anisotropyStrength, out float2 anisotropyDirection)
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

    transmission = material.transmissionFactor;
    if (material.transmissionTexture != ~0u)
        transmission *= textures[NonUniformResourceIndex(material.transmissionTexture)].SampleLevel(textureSampler, uv, 0).r;

    specularFactor = material.specularFactor;
    if (material.specularTexture != ~0u)
        specularFactor *= textures[NonUniformResourceIndex(material.specularTexture)].SampleLevel(textureSampler, uv, 0).a;

    specularColorFactor = material.specularColorFactor;
    if (material.specularColorTexture != ~0u)
        specularColorFactor *= textures[NonUniformResourceIndex(material.specularColorTexture)].SampleLevel(textureSampler, uv, 0).rgb;

    float c = cos(material.anisotropyRotation);
    float s = sin(material.anisotropyRotation);
    anisotropyStrength = material.anisotropyStrength;
    anisotropyDirection = float2(c, s);
    if (material.anisotropyTexture != ~0u)
    {
        float3 anisotropyTex = textures[NonUniformResourceIndex(material.anisotropyTexture)].SampleLevel(textureSampler, uv, 0).rgb;
        float2 texDirection = anisotropyTex.rg * 2.0f - float2(1.0f);
        texDirection = LengthSquared(texDirection) > 1e-8f ? normalize(texDirection) : float2(1.0f, 0.0f);
        anisotropyDirection = float2(c * texDirection.x - s * texDirection.y,
                                     s * texDirection.x + c * texDirection.y);
        anisotropyStrength *= anisotropyTex.b;
    }
}

Payload SamplePayload(HitPayload payload, RayDesc ray)
{
    if (!payload.IsHit()) // Misuse
        return {};
    GSInstance inst = LoadSceneInstance(payload.instance);
    Payload res;
    res.materialID = inst.materialIndex;
    res.hit = GetHitState(inst.meshOffset, payload.primitive, payload.bary, inst, ray.Origin, ray.Direction, payload.hitT);
    GSMaterial material = LoadMaterial(res.materialID);
    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    float2 anisotropyDirection;
    SampleMaterials(material, res.hit.uv, baseColor4, normalTBN, pbrMetalRough, emissive4,
                    res.transmission, res.specularFactor, res.specularColorFactor,
                    res.anisotropyStrength, anisotropyDirection);
    res.baseColor = baseColor4.xyz;
    res.emissive = emissive4.xyz;
    res.roughness = pbrMetalRough.y;
    res.metallic = pbrMetalRough.z;
    // Apply normal map in tangent space
    SetHitShadingNormal(res.hit, res.hit.tangent * normalTBN.x + res.hit.bitangent * normalTBN.y + res.hit.normal * normalTBN.z);
    RotateHitTangentFrame(res.hit, anisotropyDirection);
    res.ior = material.ior;
    res.subsurface = material.subsurfaceFactor;
    res.subsurfaceScale = material.subsurfaceScale;
    res.subsurfaceColor = material.subsurfaceColor;
    res.subsurfaceRadius = material.subsurfaceRadius;
    return res;
}

Payload SampleCurvePayload(HitPayload payload, RayDesc ray)
{
    if (!payload.IsHit())
        return {};

    GSCurveInstance inst = LoadSceneCurveInstance(GetCurveIndex(payload.instance));
    Payload res;
    res.materialID = inst.materialIndex;
    res.hit = GetCurveHitState(inst.curveOffset, payload.primitive, payload.bary, inst,
                               ray.Origin, ray.Direction, payload.hitT);
    GSMaterial material = LoadMaterial(res.materialID);
    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    float2 anisotropyDirection;
    SampleMaterials(material, res.hit.uv, baseColor4, normalTBN, pbrMetalRough, emissive4,
                    res.transmission, res.specularFactor, res.specularColorFactor,
                    res.anisotropyStrength, anisotropyDirection);
    res.baseColor = baseColor4.xyz;
    res.emissive = emissive4.xyz;
    res.roughness = pbrMetalRough.y;
    res.metallic = pbrMetalRough.z;
    SetHitShadingNormal(res.hit, res.hit.tangent * normalTBN.x + res.hit.bitangent * normalTBN.y + res.hit.normal * normalTBN.z);
    RotateHitTangentFrame(res.hit, anisotropyDirection);
    res.ior = material.ior;
    res.subsurface = 0.0f;
    res.subsurfaceScale = material.subsurfaceScale;
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
    float2 uv = float2(pixel + jitter + 0.5f) / float2(globalParams.fbWidth, globalParams.fbHeight);
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

uint SERHashCombine(uint hash, uint value)
{
    return hash * 31u + value;
}

uint GetSERMaterialFamily(GSMaterial material, bool isCurve)
{
    if (!isCurve &&
        material.subsurfaceFactor > EPS &&
        material.subsurfaceScale > EPS &&
        MaxComponent(material.subsurfaceRadius) > EPS)
        return 3u;

    if (material.transmissionFactor > EPS || material.transmissionTexture != ~0u)
        return 2u;

    if (material.metallicFactor > 0.5f ||
        material.roughnessFactor < 0.25f ||
        material.specularFactor > 1.25f ||
        material.anisotropyStrength > EPS ||
        material.anisotropyTexture != ~0u)
        return 1u;

    return 0u;
}

uint GetSERMaterialFeatureFlags(GSMaterial material)
{
    uint flags = 0u;
    if (material.baseColorTexture != ~0u ||
        material.metallicRoughnessTexture != ~0u ||
        material.transmissionTexture != ~0u ||
        material.specularTexture != ~0u ||
        material.specularColorTexture != ~0u)
        flags |= 1u;
    if (material.normalTexture != ~0u)
        flags |= 2u;
    if (material.anisotropyStrength > EPS || material.anisotropyTexture != ~0u)
        flags |= 4u;
    if (MaxComponent(material.emissiveFactor) > EPS || material.emissiveTexture != ~0u)
        flags |= 8u;
    return flags;
}

uint BuildSERMaterialKey(uint materialIndex, GSMaterial material, bool isCurve, uint bounce)
{
    uint hash = 17u;
    hash = SERHashCombine(hash, GetSERMaterialFamily(material, isCurve));
    hash = SERHashCombine(hash, GetSERMaterialFeatureFlags(material));
    hash = SERHashCombine(hash, min(bounce, 3u));
    hash = SERHashCombine(hash, materialIndex);
    return hash & 0x3Fu;
}

uint BuildSERCoherenceHint(HitObject hitObj, uint bounce)
{
    if (!hitObj.IsHit())
        return 0u;

    uint instanceID = hitObj.GetInstanceID();
    if (IsLightInstance(instanceID))
    {
        GSLight light = LoadSceneLight(GetLightIndex(instanceID));
        uint hash = SERHashCombine(17u, light.type);
        hash = SERHashCombine(hash, min(bounce, 3u));
        return (1u << 6) | (hash & 0x3Fu);
    }

    bool isCurve = IsCurveInstance(instanceID);
    uint materialIndex;
    if (isCurve)
    {
        GSCurveInstance inst = LoadSceneCurveInstance(GetCurveIndex(instanceID));
        materialIndex = inst.materialIndex;
    }
    else
    {
        GSInstance inst = LoadSceneInstance(instanceID);
        materialIndex = inst.materialIndex;
    }

    GSMaterial material = LoadMaterial(materialIndex);
    uint hitClass = isCurve ? 3u : 2u;
    return (hitClass << 6) | BuildSERMaterialKey(materialIndex, material, isCurve, bounce);
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
             kMeshMask | kCurveMask, kRayTypeShadow, 1, 1, ray, payload);
    return payload.hit;
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
    IBSSRDF bssrdf,
    inout S rng,
    out BSSRDFSurfaceSample result)
{
    result = BSSRDFSurfaceSample();

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
    float lightTotalWeight,
    out LightSampleContext ctx,
    out LightLiSample Li)
{
    ctx = LightSampleContext(hit.position, hit.geometryNormal, hit.normal);
    Li = SampleSceneLight(ctx, env, rng.sample(), rng.sample2D(), lightTotalWeight);
    if (Li.pdf <= EPS)
        return false;

    float3 shadowNormalFF = dot(Li.wi, hit.geometryNormal) < 0.0f ? -hit.geometryNormal : hit.geometryNormal;
    float3 shadowOrigin = OffsetRay(hit.position, shadowNormalFF);
    if (TraceShadowRay(shadowOrigin, Li.wi, Li.dist))
        Li.pdf = 0.0f;

    return Li.pdf > EPS;
}

void BSDF_f(IBSDF bsdf, float3 wo, float3 wi, TransportMode mode,
            out float3 fDiffuse, out float3 fSpecular)
{
    bsdf.f(wo, wi, mode, fDiffuse, fSpecular);
}

SampledSpectrum BSDF_f(IBSDF bsdf, float3 wo, float3 wi, TransportMode mode)
{
    return bsdf.f(wo, wi, mode);
}

float BSDF_PDF(IBSDF bsdf, float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags)
{
    return bsdf.PDF(wo, wi, mode, flags);
}

BSDFSample BSDF_SampleF(IBSDF bsdf, float3 wo, float uc, float2 u, TransportMode mode,
                        BxDFReflTransFlags flags, out AOVCategory category)
{
    return bsdf.Sample_f(wo, uc, u, mode, flags, category);
}

void AddLocalDirectLighting<S : ISampler>(
    Payload surface,
    IBSDF bsdf,
    float3 wo,
    float3 beta,
    uint bounce,
    AOVCategory firstBounceCategory,
    inout S rng,
    inout DirectLightingState lighting,
    inout PathRadiance radiance)
{
    LightSampleContext ctx;
    LightLiSample Li;
    if (!SampleVisibleSceneLight(surface.hit, lighting.env, rng, lighting.lightTotalWeight, ctx, Li))
    {
        lighting.lastLightCtx = ctx;
        return;
    }

    lighting.lastLightCtx = ctx;
    float3 wi = WorldToShadingLocal(surface.hit, Li.wi);
    float bsdfPdf = BSDF_PDF(bsdf, wo, wi, TransportMode::Radiance, BxDFReflTransFlags::All);
    float mis = Li.isDelta ? 1.0f : PowerHeuristic(1, Li.pdf, 1, bsdfPdf);

    if (bounce == 0)
    {
        float3 fD, fS;
        BSDF_f(bsdf, wo, wi, TransportMode::Radiance, fD, fS);
        float3 scale = beta * AbsCosTheta(wi) * Li.Li * mis;
        AddRadiance(radiance.diffuse, fD * scale, true);
        AddRadiance(radiance.specular, fS * scale, true);
    }
    else
    {
        float3 f = BSDF_f(bsdf, wo, wi, TransportMode::Radiance) * AbsCosTheta(wi);
        AddRadianceByCategory(radiance, beta * f * Li.Li * mis,
                              bounce, firstBounceCategory, AOVCategory::Diffuse, false);
    }
}

void AddDiffuseDirectLighting<S : ISampler>(
    HitState hit,
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
    if (!SampleVisibleSceneLight(hit, lighting.env, rng, lighting.lightTotalWeight, ctx, Li))
    {
        lighting.lastLightCtx = ctx;
        return;
    }

    lighting.lastLightCtx = ctx;
    float3 wi = WorldToShadingLocal(hit, Li.wi);
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

void WritePrimaryPickID(uint2 pix, uint32_t pickID)
{
    pickIDBuffer[pix] = pickID;
}

void AccumulatePathRadiance(uint2 pix, PathRadiance radiance, float frameCount)
{
    float4 outD = float4(radiance.diffuse, 1.0f);
    float4 outS = float4(radiance.specular, 1.0f);

    if (frameCount == 1) {
        diffuse[pix]  = outD;
        specular[pix] = outS;
    } else {
        float invN = 1.0f / frameCount;
        diffuse[pix]  += (outD - diffuse[pix])  * invN;
        specular[pix] += (outS - specular[pix]) * invN;
    }
}

bool AddAnalyticLightHitEmission(
    HitRecord hit,
    float3 rayDirection,
    float3 view,
    float3 beta,
    uint bounce,
    AOVCategory firstBounceCategory,
    float lastSamplePDF,
    DirectLightingState lighting,
    inout PathRadiance radiance,
    out float3 hitPoint)
{
    hitPoint = hit.position;
    if (!IsAnalyticLightHit(hit.kind))
        return false;

    GSLight gl = LoadSceneLight(hit.lightIndex);

    SampledSpectrum Lemit = SampledSpectrum(gl.color * gl.power);
    SampledSpectrum Le = SampledSpectrum(0.0f);
    float pdfLi = 0.0f;

    if (hit.kind == HitKind::Disk)
    {
        DiskLight light = DiskLight(Lemit, gl.position, gl.dpdu, gl.dpdv, gl.radius, gl.twoSided != 0);
        float3 normal = normalize(cross(gl.dpdu, gl.dpdv));
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, rayDirection, true);
    }
    else if (hit.kind == HitKind::Rect)
    {
        RectLight light = RectLight(Lemit, gl.position, gl.dpdu, gl.dpdv, gl.twoSided != 0);
        float3 normal = normalize(cross(gl.dpdu, gl.dpdv));
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, rayDirection, true);
    }

    if (any(Le > 0.0f))
    {
        float pLightSelection = lighting.lightTotalWeight > 0.0f
            ? (gl.selectionWeight / lighting.lightTotalWeight)
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
    float pdf = lighting.lightTotalWeight > 0.0f
        ? (envWeight / lighting.lightTotalWeight) * lighting.env.PDF_Li(lighting.lastLightCtx, -view, true)
        : 0.0f;
    float mis = lastSamplePDF > EPS ? PowerHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
    float3 contrib = beta * lighting.env.Le(-view) * mis;
    AddRadianceByCategory(radiance, contrib, bounce, firstBounceCategory, AOVCategory::Specular, false);
}

[shader("raygeneration")]
void RayGeneration()
{
    ClearPTTile();

    uint2 pix = GetPTPixel();
    if (!IsPTPixelInBounds(pix))
        return;

    for (uint subSample = 0; subSample < GetPTSamplesPerDispatch(); subSample++)
    {
        uint sampleIndex = GetPTBasePixelSampleCount() + subSample;

        // PCG or Sobol
        if ((ptCompileOptions & kPTCompileOptionSamplerPCG) != 0)
        {
            PCG rng = PCG(pix, sampleIndex);
            PathTrace(pix, subSample, rng);
        }
        else
        {
            Sobol rng = Sobol(pix, sampleIndex, sobolMatrices);
            PathTrace(pix, subSample, rng);
        }
    }
}

void PathTrace<S : ISampler>(uint2 pix, uint subSample, inout S rng)
{
    float3 beta = float3(1);
    PathRadiance radiance = PathRadiance();
    AOVCategory firstBounceCategory = AOVCategory::Undefined;
    bool pickingWritten = false;
    float frameCount = float(GetPTPixelSampleCount(subSample));    
    float lastSamplePDF = 0.0f;
    InfiniteLight env = InfiniteLight(
        SampledSpectrum(globalParams.ambientColor * globalParams.ambientPower),
        globalParams.envMapScale,
        globalParams.useEnvMap != 0u,
        globalParams.envAzimuthOffset
    );
    float lightTotalWeight = globalParams.sceneLightWeightSum + GetEnvLightSelectionWeight();
    DirectLightingState lighting = DirectLightingState(env, lightTotalWeight);
    uint diffuseBounces = 0;
    uint specularBounces = 0;
    uint transmissionBounces = 0;
    RayDesc ray = GenerateCameraRay(pix, rng);
    for (uint bounce = 0; ; bounce++) {
        HitPayload payload;
        if ((ptCompileOptions & kPTCompileOptionShaderExecutionReordering) != 0 /* SER */) {
            // SER
            // https://www.khronos.org/blog/boosting-ray-tracing-performance-with-shader-execution-reordering-introducing-vk-ext-ray-tracing-invocation-reorder
            // https://docs.vulkan.org/samples/latest/samples/extensions/ray_tracing_invocation_reorder/README.html
            HitObject hitObj = TraceHitObject(ray);
            uint coherenceHint = BuildSERCoherenceHint(hitObj, bounce);
            ReorderThread(hitObj, coherenceHint, kSERCoherenceHintBits);
            // Invoke closest hit / miss to populate the payload
            HitObject::Invoke(AS, hitObj, payload);
        } else
            TraceRay(AS, 0, 0xFF, kRayTypeSurface, 0, 0, ray, payload);
        // Outgoing direction
        float3 view = -ray.Direction;
        HitRecord hit = MakeHitRecord(payload, ray);

        float3 hitPoint;
        if (AddAnalyticLightHitEmission(hit, ray.Direction, view, beta, bounce, firstBounceCategory,
                                        lastSamplePDF, lighting, radiance, hitPoint))
        {
            ray.Origin = hitPoint + ray.Direction * EPS_TMin;
            bounce--; // Don't count this as a bounce
            continue;
        }

        if (hit.kind == HitKind::None)
        {
            AddEnvironmentMiss(view, beta, bounce, firstBounceCategory, lastSamplePDF, lighting, radiance);
            break;
        }
        if (!IsMaterialSurfaceHit(hit.kind))
            break;

        Payload surface = hit.kind == HitKind::Curve ? SampleCurvePayload(hit.raw, ray) : SamplePayload(hit.raw, ray);

        if (bounce == 0 && !pickingWritten)
        {
            WritePrimaryPickID(pix, hit.raw.instance);
            pickingWritten = true;
        }
        // Local frame setup
        float3 wo = WorldToShadingLocal(surface.hit, view);
        IMaterial material = MakeMaterial(surface, wo);
        // Subsurface events are non-local. The closure picks the event
        // the integrator performs the scene query.
        float pSampleSubsurface = material.ProbSampleSubsurface();
        float pSampleSurface = material.ProbSampleSurface();
        pSampleSurface = max(pSampleSurface, EPS);
        bool sampleSubsurface = rng.sample() < pSampleSubsurface;
        PathSample nextSample = PathSample();
        // TODO Volumetrics
        if (sampleSubsurface)
        {
            BSSRDFSurfaceSample sample;
            IBSSRDF bssrdf = material.GetSubsurface();
            if (!SeparableBSSRDF_SampleSp(surface, hit.raw.instance, bssrdf, rng, sample))
                break;

            float3 localBeta = beta * sample.Sp / max(pSampleSubsurface * sample.pdf, EPS);
            if (any(isnan(localBeta)) || any(isinf(localBeta)))
                break;

            Payload exitSurface = sample.surface;
            // Lambertian assumed for subsurface interactions
            const float3 exitF = float3(InvPi);
            AddDiffuseDirectLighting(exitSurface.hit, exitF, localBeta,
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
        }
        else
        {
            float3 localBeta = beta / pSampleSurface;
            IBSDF bsdf = material.GetSurface();
            AddLocalDirectLighting(surface, bsdf, wo, localBeta,
                                   bounce, firstBounceCategory, rng, lighting, radiance);
            AddSurfaceEmissive(surface, localBeta, bounce, firstBounceCategory, radiance);

            AOVCategory sampleCategory;
            BSDFSample sample = BSDF_SampleF(bsdf, wo, rng.sample(), rng.sample2D(),
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

        ray.Direction = normalize(ShadingLocalToWorld(nextSample.hit, nextSample.wi));
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
    if (!pickingWritten)
        WritePrimaryPickID(pix, ~0u);

    AccumulatePathRadiance(pix, radiance, frameCount);
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
        HitState hs = GetHitState(inst.meshOffset, primitive, attr.barycentrics.xy, inst, WorldRayOrigin(), WorldRayDirection(), RayTCurrent());
        baseColor *= textures[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(textureSampler, hs.uv, 0);
    }
    float opacity = baseColor.a;
    if (opacity == 1.0f)
        return false;

    if (rng.sample() < 1.0f - opacity)
        return true;
    return false;
}

struct ProceduralHitAttributes
{
    float2 localPosition;
};

bool IntersectUnitLightPlane(out float t, out float2 localPosition)
{
    float3 ro = ObjectRayOrigin();
    float3 rd = ObjectRayDirection();
    t = 0.0f;
    localPosition = float2(0.0f);
    if (abs(rd.z) < 1e-7f)
        return false;

    t = -ro.z / rd.z;
    if (t < RayTMin() || t > RayTCurrent())
        return false;

    float3 p = ro + rd * t;
    localPosition = p.xy;
    return true;
}

[shader("intersection")]
void RectLightIntersection()
{
    float t;
    float2 p;
    if (!IntersectUnitLightPlane(t, p))
        return;
    if (any(abs(p) > 1.0f))
        return;

    ProceduralHitAttributes attr;
    attr.localPosition = p;
    ReportHit(t, 0, attr);
}

[shader("intersection")]
void DiskLightIntersection()
{
    float t;
    float2 p;
    if (!IntersectUnitLightPlane(t, p))
        return;
    if (dot(p, p) > 1.0f)
        return;

    ProceduralHitAttributes attr;
    attr.localPosition = p;
    ReportHit(t, 0, attr);
}

void FillProceduralLightHit(inout HitPayload payload, ProceduralHitAttributes attr)
{
    payload.instance = InstanceID();
    payload.primitive = PrimitiveIndex();
    payload.bary = attr.localPosition;
    payload.hitT = RayTCurrent();
}

struct CurveHitAttributes
{
    float2 uv;
};

bool TryUpdateCurveHit(float t, float u, inout float bestT, inout float bestU)
{
    if (t < RayTMin() || t > RayTCurrent() || t >= bestT)
        return false;
    bestT = t;
    bestU = u;
    return true;
}

void TryIntersectCurveCap(float3 ro, float3 rd, float3 center, float radius, float u,
                          inout float bestT, inout float bestU)
{
    float3 oc = ro - center;
    float a = dot(rd, rd);
    float b = dot(oc, rd);
    float c = dot(oc, oc) - radius * radius;
    float h = b * b - a * c;
    if (h < 0.0f || a <= 1e-12f)
        return;

    float root = sqrt(h);
    float t = (-b - root) / a;
    if (!TryUpdateCurveHit(t, u, bestT, bestU))
        TryUpdateCurveHit((-b + root) / a, u, bestT, bestU);
}

[shader("intersection")]
void CurveIntersection()
{
    GSCurveInstance inst = LoadSceneCurveInstance(GetCurveIndex(InstanceID()));
    GSCurveSet curve = primitives.Load<GSCurveSet>(inst.curveOffset);
    GSCurveSegment segment = primitives.Load<GSCurveSegment>(curve.segmentOffset + PrimitiveIndex() * sizeof(GSCurveSegment));
    GSCurvePoint p0 = primitives.Load<GSCurvePoint>(curve.pointOffset + segment.p0 * sizeof(GSCurvePoint));
    GSCurvePoint p1 = primitives.Load<GSCurvePoint>(curve.pointOffset + segment.p1 * sizeof(GSCurvePoint));

    float maxRadius = max(p0.radius, p1.radius);
    if (maxRadius <= 0.0f)
        return;

    float3 ro = ObjectRayOrigin();
    float3 rd = ObjectRayDirection();
    float3 ba = p1.position - p0.position;
    float3 oa = ro - p0.position;
    float baba = dot(ba, ba);

    float bestT = RayTCurrent();
    float bestU = segment.u0;

    if (baba > 1e-12f)
    {
        float bard = dot(ba, rd);
        float baoa = dot(ba, oa);
        float3 rdPerp = rd - ba * (bard / baba);
        float3 oaPerp = oa - ba * (baoa / baba);
        float r0 = p0.radius;
        float radiusDelta = p1.radius - p0.radius;
        float k0 = r0 + radiusDelta * baoa / baba;
        float k1 = radiusDelta * bard / baba;
        float a = dot(rdPerp, rdPerp) - k1 * k1;
        float b = dot(oaPerp, rdPerp) - k0 * k1;
        float c = dot(oaPerp, oaPerp) - k0 * k0;
        float h = b * b - a * c;
        if (h >= 0.0f && abs(a) > 1e-12f)
        {
            float t = (-b - sqrt(h)) / a;
            float y = baoa + t * bard;
            if (y >= 0.0f && y <= baba)
            {
                float u = y / baba;
                TryUpdateCurveHit(t, lerp(segment.u0, segment.u1, u), bestT, bestU);
            }
        }
    }

    TryIntersectCurveCap(ro, rd, p0.position, p0.radius, segment.u0, bestT, bestU);
    TryIntersectCurveCap(ro, rd, p1.position, p1.radius, segment.u1, bestT, bestU);

    if (bestT < RayTCurrent())
    {
        CurveHitAttributes attr;
        attr.uv = float2(bestU, 0.0f);
        ReportHit(bestT, 0, attr);
    }
}

[shader("closesthit")]
void RectLightClosestHit(inout HitPayload payload, in ProceduralHitAttributes attr)
{
    FillProceduralLightHit(payload, attr);
}

[shader("closesthit")]
void DiskLightClosestHit(inout HitPayload payload, in ProceduralHitAttributes attr)
{
    FillProceduralLightHit(payload, attr);
}

[shader("closesthit")]
void CurveClosestHit(inout HitPayload payload, in CurveHitAttributes attr)
{
    payload.instance = InstanceID();
    payload.primitive = PrimitiveIndex();
    payload.bary = attr.uv;
    payload.hitT = RayTCurrent();
}

[shader("anyhit")]
void CurveShadowAnyHit(inout ShadowPayload payload, in CurveHitAttributes attr)
{
    payload.hit = true;
    AcceptHitAndEndSearch();
}

[shader("anyhit")] // Hit Group 0
void RayOpacityAnyHit(in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceID();
    uint primitive = PrimitiveIndex();
    PCG rng = PCG(GetPTPixel(), globalParams.frameNumber, hash(uint2(instance, primitive)));
    if (AnyHitOpacityIgnore(rng, instance, primitive, attr))
        return IgnoreHit();
}

[shader("anyhit")] // Hit Group 1
void ShadowRayAnyHit(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instance = InstanceID();
    uint primitive = PrimitiveIndex();
    PCG rng = PCG(GetPTPixel(), globalParams.frameNumber, hash(uint2(instance, primitive)));
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

    PCG rng = PCG(GetPTPixel(), globalParams.frameNumber,
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