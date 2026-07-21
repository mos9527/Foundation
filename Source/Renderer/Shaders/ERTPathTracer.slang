// References
// - PBRT v4
// - RAY TRACING GEMS II - Chapter 14
// - https://github.com/boksajak/referencePT/blob/master/shaders/PathTracer.hlsl
// - https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang
// And many more. Respective includes will tell.
import IBindless2D;
import IFlags;
import ICommon;
import ILight;
import IBSDF;
import IBSSRDF;
import IMaterial;
import IMath;
import IBxDF;

// Packed path-tracer compile options. Must match Renderer.hpp.
static const uint kPTCompileOptionSamplerSobol = 1u << 1;
static const uint kPTCompileOptionSamplerPCG = 1u << 2;
static const uint kPTCompileOptionForceTextureLOD0 = 1u << 3;
static const uint kPTCompileOptionLightSamplerUniform = 1u << 4;
static const uint kPTCompileOptionEnergyCompensation = 1u << 5;
[[SpecializationConstant]] uint ptCompileOptions;

uniform UBO globalParams;

// -- Scene
RaytracingAccelerationStructure AS;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitives;
ByteAddressBuffer dynamicPrimitives; // CPU-updateable geometry ring (current frame slot); aliases `primitives` when the feature is off
StructuredBuffer<GSMaterial> materials;
StructuredBuffer<GSLight> lights;
#include "ILightBVH.slang"
SamplerState textureSampler;
StructuredBuffer<uint> sobolMatrices;
// -- Path radiance accumulation buffers
RWTexture2D<float4> diffuse;            // Diffuse lobe radiance
RWTexture2D<float4> specular;           // Specular lobe radiance (incl. transmission)
RWTexture2D<float> depth;               // NDC depth
// -- Debug buffers
RWTexture2D<uint32_t> instanceIDBuffer; // Instance ID map, ~0u = no object (written on primary hit only)
RWTexture2D<float4> adaptiveAux; // xyz = Half-samples, w = Convergence flag

// SER coherence hint layout:
//   [7:6] hit class (miss, analytic light, triangle, curve)
//   [5:0] mixed material/light key
static const uint kSERCoherenceHintBits = 8u;

static const uint kRayTypeSurface = 0u;
static const uint kRayTypeShadow = 1u;
static const uint kRayTypeBSSRDFQuery = 2u;
static const uint32_t kLightInstanceBit = 1u << 23;
static const uint32_t kGSInstanceTypeMesh = 0u;
static const uint32_t kGSInstanceTypeCurve = 1u;
static const uint kMeshMask = 0x01;
static const uint kCurveMask = 0x04;



// Hit state helpers
struct RayCone
{
    float width;
    float spreadAngle;
};

struct CameraRay
{
    RayDesc ray;
    RayCone rayCone;
};

struct HitState
{
    float3 position;
    float3 normal;
    float3 geometryNormal;
    float3 tangent;
    float3 bitangent;
    float2 uv;
    float2 uvDX;
    float2 uvDY;
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
        CoordinateSystem(hit.normal, tangent, fallbackBitangent);
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
    return globalParams.instances.offset + localInstanceIndex;
}

uint GetMaterialBufferIndex(uint localMaterialIndex)
{
    return globalParams.materials.offset + localMaterialIndex;
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

bool IsCurveInstance(uint instance)
{
    return LoadSceneInstance(instance).type == kGSInstanceTypeCurve;
}

GSMaterial LoadMaterial(uint localMaterialIndex)
{
    return materials[NonUniformResourceIndex(GetMaterialBufferIndex(localMaterialIndex))];
}

GSLight LoadSceneLight(uint localLightIndex)
{
    return lights[NonUniformResourceIndex(globalParams.lights.offset + localLightIndex)];
}

void ApplyRayConeAnisotropy(inout HitState hit, GSInstance inst,
                            float3 p0, float3 p1, float3 p2,
                            float2 u0, float2 u1, float2 u2,
                            float3 rayDirection,
                            float rayConeWidth)
{
    float3 rayConeDir = rayDirection;
    float rayConeRadiusAtIntersection = rayConeWidth * 0.5f;

    float3 ellipseAxis0 = rayConeDir - dot(hit.geometryNormal, rayConeDir) * hit.geometryNormal;
    float3 rayDirPlaneProjection0 = ellipseAxis0 - dot(rayConeDir, ellipseAxis0) * rayConeDir;
    float len0 = length(rayDirPlaneProjection0);
    ellipseAxis0 *= rayConeRadiusAtIntersection / max(0.0001f, len0);

    float3 ellipseAxis1 = cross(hit.geometryNormal, ellipseAxis0);
    float3 rayDirPlaneProjection1 = ellipseAxis1 - dot(rayConeDir, ellipseAxis1) * rayConeDir;
    float len1 = length(rayDirPlaneProjection1);
    ellipseAxis1 *= rayConeRadiusAtIntersection / max(0.0001f, len1);

    // Transform points to world space
    float3 wp0 = inst.transform + inst.rotation.rotate(p0 * inst.scale);
    float3 wp1 = inst.transform + inst.rotation.rotate(p1 * inst.scale);
    float3 wp2 = inst.transform + inst.rotation.rotate(p2 * inst.scale);
    
    float3 d = hit.position - wp0;
    float3 edge01 = wp1 - wp0;
    float3 edge02 = wp2 - wp0;
    float oneOverAreaTriangle = 1.0f / dot(hit.geometryNormal, cross(edge01, edge02));

    float3 edgeP;
    float u, v;
    
    edgeP = d + ellipseAxis0;
    u = dot(hit.geometryNormal, cross(edgeP, edge02)) * oneOverAreaTriangle;
    v = dot(hit.geometryNormal, cross(edge01, edgeP)) * oneOverAreaTriangle;
    hit.uvDX = (1.0f - u - v) * u0 + u * u1 + v * u2 - hit.uv;

    edgeP = d + ellipseAxis1;
    u = dot(hit.geometryNormal, cross(edgeP, edge02)) * oneOverAreaTriangle;
    v = dot(hit.geometryNormal, cross(edge01, edgeP)) * oneOverAreaTriangle;
    hit.uvDY = (1.0f - u - v) * u0 + u * u1 + v * u2 - hit.uv;
}

// `prims` selects the static or dynamic primitive buffer (Slang specializes per buffer). The
// caller branches on GSInstanceIsDynamic(inst); for dynamic geo `meshOffset` is already the
// current frame slot's absolute header offset (resolved in EndScene).
HitState GetHitState(ByteAddressBuffer prims, uint meshOffset, uint primitive, float2 bary, GSInstance inst,
                     float3 rayOrigin, float3 rayDirection, float hitT,
                     float rayConeWidth)
{
    HitState hit;
    hit.uvDX = float2(0.0f);
    hit.uvDY = float2(0.0f);

    // Construct position from ray equation (more robust than interpolation + transform)
    hit.position  = rayOrigin + rayDirection * hitT;

    // Barycentric weights
    const float3 barycentrics = float3(1.0 - bary.x - bary.y, bary.x, bary.y);

    // Fetch triangle indices
    GSMesh mesh = prims.Load<GSMesh>(meshOffset);
    uint i0 = prims.Load<uint32_t>(mesh.indices.offset + (primitive * 3 + 0) * sizeof(uint32_t));
    uint i1 = prims.Load<uint32_t>(mesh.indices.offset + (primitive * 3 + 1) * sizeof(uint32_t));
    uint i2 = prims.Load<uint32_t>(mesh.indices.offset + (primitive * 3 + 2) * sizeof(uint32_t));

    // Fetch and decode vertices
    FQVertex vq0 = prims.Load<FQVertex>(mesh.vertices.offset + i0 * sizeof(FQVertex));
    FQVertex vq1 = prims.Load<FQVertex>(mesh.vertices.offset + i1 * sizeof(FQVertex));
    FQVertex vq2 = prims.Load<FQVertex>(mesh.vertices.offset + i2 * sizeof(FQVertex));
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
    
    if (rayConeWidth > 0.0f)
        ApplyRayConeAnisotropy(hit, inst, p0, p1, p2, u0, u1, u2, rayDirection, rayConeWidth);
    return hit;
}

HitState GetHitState(ByteAddressBuffer prims, uint meshOffset, uint primitive, float2 bary, GSInstance inst, float3 rayOrigin, float3 rayDirection, float hitT)
{
    return GetHitState(prims, meshOffset, primitive, bary, inst, rayOrigin, rayDirection, hitT, 0.0f);
}

// Buffer-selecting wrappers: dynamic geometry reads from `dynamicPrimitives`, everything else
// from the immutable `primitives`. The branch picks one statically-specialized instantiation.
HitState GetHitState(uint meshOffset, uint primitive, float2 bary, GSInstance inst, float3 rayOrigin, float3 rayDirection, float hitT, float rayConeWidth)
{
    if (GSInstanceIsDynamic(inst))
        return GetHitState(dynamicPrimitives, meshOffset, primitive, bary, inst, rayOrigin, rayDirection, hitT, rayConeWidth);
    return GetHitState(primitives, meshOffset, primitive, bary, inst, rayOrigin, rayDirection, hitT, rayConeWidth);
}
HitState GetHitState(uint meshOffset, uint primitive, float2 bary, GSInstance inst, float3 rayOrigin, float3 rayDirection, float hitT)
{
    return GetHitState(meshOffset, primitive, bary, inst, rayOrigin, rayDirection, hitT, 0.0f);
}

float2 GetHitUV(ByteAddressBuffer prims, uint meshOffset, uint primitive, float2 bary)
{
    const float3 barycentrics = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    GSMesh mesh = prims.Load<GSMesh>(meshOffset);
    uint i0 = prims.Load<uint32_t>(mesh.indices.offset + (primitive * 3 + 0) * sizeof(uint32_t));
    uint i1 = prims.Load<uint32_t>(mesh.indices.offset + (primitive * 3 + 1) * sizeof(uint32_t));
    uint i2 = prims.Load<uint32_t>(mesh.indices.offset + (primitive * 3 + 2) * sizeof(uint32_t));

    float2 u0 = float2(prims.Load<FQVertex>(mesh.vertices.offset + i0 * sizeof(FQVertex)).uv);
    float2 u1 = float2(prims.Load<FQVertex>(mesh.vertices.offset + i1 * sizeof(FQVertex)).uv);
    float2 u2 = float2(prims.Load<FQVertex>(mesh.vertices.offset + i2 * sizeof(FQVertex)).uv);
    
    return u0 * barycentrics.x + u1 * barycentrics.y + u2 * barycentrics.z;
}

float2 GetHitUV(uint meshOffset, uint primitive, float2 bary, GSInstance inst)
{
    if (GSInstanceIsDynamic(inst))
        return GetHitUV(dynamicPrimitives, meshOffset, primitive, bary);
    return GetHitUV(primitives, meshOffset, primitive, bary);
}

// Infinite tapered-cone intersector (Inigo Quilez, MIT) with origin re-centering
// for numerical stability (Ray Tracing Gems II, ch. 34).
float4 IntersectInfiniteCone(float3 ro, float3 rd, float3 pa, float3 pb, float ra, float rb)
{
    float3 oa = ro - pa;
    float tOffset = -dot(rd, oa);
    float3 roOffset = tOffset * rd;
    oa = ro + roOffset - pa;

    float3 ba = pb - pa;
    float rr = ra - rb;
    float m0 = dot(ba, ba);
    float m1 = dot(ba, oa);
    float m2 = dot(ba, rd);
    float m3 = dot(rd, oa);
    float m5 = dot(oa, oa);
    float d2 = m0 - rr * rr;

    float k2 = d2 - m2 * m2;
    float k1 = d2 * m3 - m1 * m2 + m2 * rr * ra;
    float k0 = d2 * m5 - m1 * m1 + m1 * rr * ra * 2.0f - m0 * ra * ra;

    float h = k1 * k1 - k0 * k2;
    if (h < 0.0f)
        return float4(-1.0f, 0.0f, 0.0f, 0.0f);

    float t = (-sqrt(h) - k1) / k2;
    if (t + tOffset < 0.0f)
        return float4(-1.0f, 0.0f, 0.0f, 0.0f);

    float y = m1 - ra * rr + t * m2;
    float3 hitPoint = d2 * (oa + t * rd);
    float3 axisPoint = ba * y;
    float3 normal = normalize(hitPoint - axisPoint);
    return float4(t + tOffset, normal);
}

GSCurveLeaf LoadCurveLeaf(GSCurveSet curve, uint primitive)
{
    uint leafIndex = primitive / 4u;
    return primitives.Load<GSCurveLeaf>(curve.leaves.offset + leafIndex * sizeof(GSCurveLeaf));
}

HitState GetCurveHitState(uint curveOffset, uint primitive, float2 /*triangleBary*/, GSInstance inst,
                          float3 rayOrigin, float3 rayDirection, float hitT)
{
    HitState hit;
    hit.uvDX = float2(0.0f);
    hit.uvDY = float2(0.0f);

    GSCurveSet curve = primitives.Load<GSCurveSet>(curveOffset);
    GSCurveLeaf leaf = LoadCurveLeaf(curve, primitive);

    float3 objRo = inst.rotation.conjugate().rotate(rayOrigin - inst.transform) / inst.scale;
    float3 objRd = inst.rotation.conjugate().rotate(rayDirection) / inst.scale;
    if (LengthSquared(objRd) <= 1e-12f)
        objRd = float3(0.0f, 0.0f, 1.0f);

    float3 axis = leaf.p1 - leaf.p0;
    float axisLen2 = LengthSquared(axis);
    float3 tangent = axisLen2 > 1e-12f ? normalize(axis) : float3(0.0f, 1.0f, 0.0f);

    float4 coneHit = IntersectInfiniteCone(objRo, objRd, leaf.p0, leaf.p1, leaf.r0, leaf.r1);
    float3 localNormal;
    float localU;
    if (coneHit.x >= 0.0f)
    {
        localNormal = coneHit.yzw;
        float3 objHit = objRo + objRd * coneHit.x;
        localU = axisLen2 > 1e-12f ? saturate(dot(objHit - leaf.p0, axis) / axisLen2) : 0.0f;
    }
    else
    {
        // Degenerate / near-parallel fallback: project the triangle hit onto the segment.
        float3 worldHit = rayOrigin + rayDirection * hitT;
        float3 objHit = inst.rotation.conjugate().rotate(worldHit - inst.transform) / inst.scale;
        localU = axisLen2 > 1e-12f ? saturate(dot(objHit - leaf.p0, axis) / axisLen2) : 0.0f;
        float3 axisPoint = lerp(leaf.p0, leaf.p1, localU);
        float3 radial = objHit - axisPoint;
        if (LengthSquared(radial) <= 1e-12f)
        {
            float3 fallback;
            CoordinateSystem(tangent, localNormal, fallback);
        }
        else
            localNormal = normalize(radial);
    }

    float radius = lerp(leaf.r0, leaf.r1, localU);
    float3 axisPoint = lerp(leaf.p0, leaf.p1, localU);
    float3 objPosition = axisPoint + localNormal * max(radius, 0.0f);
    hit.position = inst.transform + inst.rotation.rotate(objPosition * inst.scale);

    float3 viewLocal = -normalize(objRd);
    float3 facing = viewLocal - tangent * dot(viewLocal, tangent);
    float3 localBitangent;
    if (LengthSquared(facing) > 1e-12f)
    {
        facing = normalize(facing);
        localBitangent = normalize(cross(tangent, facing));
    }
    else
    {
        CoordinateSystem(tangent, facing, localBitangent);
    }
    if (dot(localNormal, facing) < 0.0f)
        localNormal = -localNormal;

    float h = clamp(dot(localNormal, localBitangent), -1.0f, 1.0f);
    hit.uv = float2(lerp(leaf.u0, leaf.u1, localU), 0.5f * (h + 1.0f));

    localBitangent = normalize(cross(localNormal, tangent));
    if (LengthSquared(localBitangent) <= 1e-12f)
        localBitangent = normalize(cross(facing, tangent));

    float3 invScale = 1.0f / inst.scale;
    hit.normal = normalize(inst.rotation.rotate(localNormal * invScale));
    hit.geometryNormal = hit.normal;
    hit.tangent = normalize(inst.rotation.rotate(tangent * inst.scale));
    hit.bitangent = normalize(inst.rotation.rotate(localBitangent * inst.scale));
    return hit;
}

static const uint32_t INSTANCE_NO_HIT = ~0u;

enum HitKind
{
    None = 0,
    Triangle = 1,
    Disk = 2,
    Rect = 3,
    Curve = 4,
    Point = 5,
    Spot = 6
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
    uint32_t type = GSLightType(light);
    if (type == kGSLightTypePoint)
        return HitKind::Point;
    if (type == kGSLightTypeSpot)
        return HitKind::Spot;
    if (type == kGSLightTypeDisk)
        return HitKind::Disk;
    if (type == kGSLightTypeRect)
        return HitKind::Rect;
    return HitKind::None;
}

bool IsAnalyticLightHit(HitKind kind)
{
    return kind == HitKind::Point || kind == HitKind::Spot ||
        kind == HitKind::Disk || kind == HitKind::Rect;
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


struct BSSRDFSurfaceSample
{
    bool valid;
    HitState surface;
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
    LightSampleContext lastLightCtx;

    __init(InfiniteLight env)
    {
        this.env = env;
        this.lastLightCtx = LightSampleContext(float3(0), float3(0), float3(0));
    }
};

// Material helpers
// vvvvv TODO More materials? Custom ones?
//       One uber shader here is surely insufficent. Need to compile ad-hoc shaders _here_.
IMaterial MakeMaterial(HitPayload payload, inout HitState hit, float3 wo, out float3 outBaseColor, bool isCurve = false, bool foldSubsurfaceToDiffuse = false)
{
    GSInstance inst = LoadSceneInstance(payload.instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);

    float4 baseColor4, normalTBN, pbrMetalRough, emissive4;
    float2 anisotropyDirection;
    float transmission, specularFactor, anisotropyStrength;
    float3 specularColorFactor;
    float sheenWeight, sheenRoughness;
    float3 sheenTint;
    float clearcoatFactor, clearcoatRoughness;

    SampleMaterials(material, hit.uv, hit.uvDX, hit.uvDY,
                    baseColor4, normalTBN, pbrMetalRough, emissive4,
                    transmission, specularFactor, specularColorFactor,
                    anisotropyStrength, anisotropyDirection,
                    sheenWeight, sheenRoughness, sheenTint,
                    clearcoatFactor, clearcoatRoughness);

    outBaseColor = baseColor4.xyz;

    SetHitShadingNormal(hit, hit.tangent * normalTBN.x + hit.bitangent * normalTBN.y + hit.normal * normalTBN.z);
    RotateHitTangentFrame(hit, anisotropyDirection);

    if (material.shaderBlockID == kMaterialShaderBlockHair)
    {
        float h = isCurve ? 2.0f * saturate(hit.uv.y) - 1.0f : 0.0f;
        return PrincipledHairMaterial(
            baseColor4.xyz, material.hairBetaM, material.hairBetaN, h, material.ior, material.hairAlpha,
            emissive4.xyz);
    }
    
    float subsurface = foldSubsurfaceToDiffuse ? 0.0f : material.subsurfaceFactor;
    return PrincipledMaterial(
        baseColor4.xyz, pbrMetalRough.z, pbrMetalRough.y,
        material.ior, specularFactor, transmission,
        (ptCompileOptions & kPTCompileOptionEnergyCompensation) != 0u, wo,
        subsurface, material.subsurfaceColor, material.subsurfaceRadius, material.subsurfaceScale,
        anisotropyStrength, specularColorFactor, globalParams.ggxLutEIndex, globalParams.ggxLutEavgIndex, globalParams.ggxLutEIORIndex, globalParams.ggxLutEIORavgIndex, globalParams.ggxLutEIORInvIndex, globalParams.ggxLutEIORInvavgIndex,
        sheenWeight, sheenRoughness, sheenTint, globalParams.sheenLtcIndex,
        clearcoatFactor, clearcoatRoughness,
        emissive4.xyz);
}


float4 SampleMaterialTexture(uint textureIndex, float2 uv, float2 uvDX, float2 uvDY)
{
    if ((ptCompileOptions & kPTCompileOptionForceTextureLOD0) != 0)
        return textures[NonUniformResourceIndex(textureIndex)].SampleLevel(textureSampler, uv, 0);
    return textures[NonUniformResourceIndex(textureIndex)].SampleGrad(textureSampler, uv, uvDX, uvDY);
}

void SampleMaterials(GSMaterial material, float2 uv, float2 uvDX, float2 uvDY,
    out float4 baseColor, out float4 normalTBN, out float4 pbrMetalRough, out float4 emissive,
    out float transmission, out float specularFactor, out float3 specularColorFactor,
    out float anisotropyStrength, out float2 anisotropyDirection,
    out float sheenWeight, out float sheenRoughness, out float3 sheenTint,
    out float clearcoatFactor, out float clearcoatRoughness)
{
    int materialFlags = globalParams.dbgMaterialFlags;
    baseColor = material.baseColorFactor;
    if (material.baseColorTexture != ~0u)
        baseColor *= SampleMaterialTexture(material.baseColorTexture, uv, uvDX, uvDY);
    if ((materialFlags & kMaterialDbgWhiteBaseColor) != 0u)
        baseColor = float4(1.0f, 1.0f, 1.0f, baseColor.a);    
    normalTBN = float4(0, 0, 1, 0);
    if (material.normalTexture != ~0u)
    {
        // glTF: blue maps to Z in [1/255, 1], so the tangent-space normal always points toward
        // the viewer. Reconstruct Z from the scaled XY (clamped) so the result stays a positive-
        // hemisphere unit normal regardless of the sampled/filtered blue channel.
        float2 xy = (SampleMaterialTexture(material.normalTexture, uv, uvDX, uvDY).xy * 2 - 1) * material.normalScale;
        normalTBN.xyz = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    }
    pbrMetalRough = float4(1, material.roughnessFactor, material.metallicFactor, 1);
    if (material.metallicRoughnessTexture != ~0u)
        pbrMetalRough *= SampleMaterialTexture(material.metallicRoughnessTexture, uv, uvDX, uvDY);
    emissive = float4(material.emissiveFactor, 0);
    if (material.emissiveTexture != ~0u)
        emissive *= SampleMaterialTexture(material.emissiveTexture, uv, uvDX, uvDY);

    transmission = material.transmissionFactor;
    if (material.transmissionTexture != ~0u)
        transmission *= SampleMaterialTexture(material.transmissionTexture, uv, uvDX, uvDY).r;

    specularFactor = material.specularFactor;
    if (material.specularTexture != ~0u)
        specularFactor *= SampleMaterialTexture(material.specularTexture, uv, uvDX, uvDY).a;

    specularColorFactor = material.specularColorFactor;
    if (material.specularColorTexture != ~0u)
        specularColorFactor *= SampleMaterialTexture(material.specularColorTexture, uv, uvDX, uvDY).rgb;

    float c = cos(material.anisotropyRotation);
    float s = sin(material.anisotropyRotation);
    anisotropyStrength = material.anisotropyStrength;
    anisotropyDirection = float2(c, s);
    if (material.anisotropyTexture != ~0u)
    {
        float3 anisotropyTex = SampleMaterialTexture(material.anisotropyTexture, uv, uvDX, uvDY).rgb;
        float2 texDirection = anisotropyTex.rg * 2.0f - float2(1.0f);
        texDirection = LengthSquared(texDirection) > 1e-8f ? normalize(texDirection) : float2(1.0f, 0.0f);
        anisotropyDirection = float2(c * texDirection.x - s * texDirection.y,
                                     s * texDirection.x + c * texDirection.y);
        anisotropyStrength *= anisotropyTex.b;
    }

    float3 sheenColor = material.sheenColorFactor;
    if (material.sheenColorTexture != ~0u)
        sheenColor *= SampleMaterialTexture(material.sheenColorTexture, uv, uvDX, uvDY).rgb;

    sheenWeight = saturate(MaxComponent(sheenColor));
    sheenTint = sheenWeight > EPS ? saturate(sheenColor / sheenWeight) : float3(1.0f);

    sheenRoughness = material.sheenRoughnessFactor;
    if (material.sheenRoughnessTexture != ~0u)
        sheenRoughness *= SampleMaterialTexture(material.sheenRoughnessTexture, uv, uvDX, uvDY).a;
    sheenRoughness = saturate(sheenRoughness);

    clearcoatFactor = material.clearcoatFactor;
    if (material.clearcoatTexture != ~0u)
        clearcoatFactor *= SampleMaterialTexture(material.clearcoatTexture, uv, uvDX, uvDY).r;
    clearcoatFactor = saturate(clearcoatFactor);

    clearcoatRoughness = material.clearcoatRoughnessFactor;
    if (material.clearcoatRoughnessTexture != ~0u)
        clearcoatRoughness *= SampleMaterialTexture(material.clearcoatRoughnessTexture, uv, uvDX, uvDY).g;
    clearcoatRoughness = saturate(clearcoatRoughness);
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

bool IsPanoramicCamera()
{
    return globalParams.cameraProjection == kCameraProjectionPanoramic;
}

float3 GeneratePanoramicRayDirection(float2 pixelSample)
{
    float2 uv = pixelSample / float2(globalParams.fbWidth, globalParams.fbHeight);
    float phi = (frac(uv.x) - 0.5f) * 2.0f * Pi;
    float theta = saturate(uv.y) * Pi;

    float sinTheta = sin(theta);
    float3 forward = normalize(globalParams.camDirection.xyz);
    float3 right = normalize(mul(globalParams.inverseView, float4(1.0f, 0.0f, 0.0f, 0.0f)).xyz);
    float3 up = normalize(mul(globalParams.inverseView, float4(0.0f, 1.0f, 0.0f, 0.0f)).xyz);
    return normalize(
        sinTheta * (cos(phi) * forward + sin(phi) * right) +
        cos(theta) * up
    );
}

// Primary ray with jitter
float3 GeneratePrimaryRayDirection(float2 pixelSample)
{
    if (IsPanoramicCamera())
        return GeneratePanoramicRayDirection(pixelSample);

    float2 uv = pixelSample / float2(globalParams.fbWidth, globalParams.fbHeight);
    float4 ndcPosition = float4(uv, 1.0f, 1.0f);
    ndcPosition.y = 1 - ndcPosition.y;
    ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
    float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);
    return normalize(wsPosition.xyz / wsPosition.w - globalParams.camPosition.xyz);
}

float WorldToNDCDepth(float3 worldPosition)
{
    float4 clipPosition = mul(mul(globalParams.proj, globalParams.view), float4(worldPosition, 1.0f));
    return clipPosition.z / clipPosition.w;
}

float3 GeneratePrimaryRay<S : ISampler>(uint2 pixel, inout S rng)
{
    float2 jitter = lerp(float2(-0.5f), float2(0.5f), rng.sample2D());
    return GeneratePrimaryRayDirection(float2(pixel) + jitter + 0.5f);
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

RayDesc MakeCameraRay(float3 direction, float2 lensSample)
{
    RayDesc ray;
    ray.Origin = globalParams.camPosition.xyz;
    ray.Direction = direction;
    ray.TMin = EPS_TMin;
    ray.TMax = INF;

    // Simple Physical Lens
    // https://www.pbr-book.org/4ed/Cameras_and_Film/Projective_Camera_Models#fragment-Computepointonplaneoffocus-0
    // https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang#L922
    if (!IsPanoramicCamera() && globalParams.aperture > EPS)
    {
        float3 focalPoint = globalParams.focalDistance * ray.Direction;
        float3 T = mul(globalParams.inverseViewProj, float4(1, 0, 0, 0)).xyz;
        float3 B = mul(globalParams.inverseViewProj, float4(0, 1, 0, 0)).xyz;
        float3 lensPoint = lensSample.x * T + lensSample.y * B;
        ray.Origin += lensPoint;
        ray.Direction = normalize(focalPoint - lensPoint);
    }

    return ray;
}

CameraRay GenerateCameraRay<S : ISampler>(uint2 pixel, inout S rng, bool debugView = false)
{
    float2 jitter = lerp(float2(-0.5f), float2(0.5f), rng.sample2D());
    float2 pixelSample = float2(pixel) + 0.5f;
    float2 lensSample = float2(0.0f);
    if (!debugView) {
        pixelSample += jitter;
        if (!IsPanoramicCamera() && globalParams.aperture > EPS)
            lensSample = SampleAnamorphicLens(rng.sample2D()) * globalParams.aperture;
    }
    CameraRay cameraRay;
    cameraRay.ray = MakeCameraRay(GeneratePrimaryRayDirection(pixelSample), lensSample);
    cameraRay.rayCone.width = 0.0f;
    float3 p0 = GeneratePrimaryRayDirection(pixelSample);
    float3 p1 = GeneratePrimaryRayDirection(pixelSample + float2(1.0f, 0.0f));
    cameraRay.rayCone.spreadAngle = acos(clamp(dot(p0, p1), -1.0f, 1.0f));

    if ((ptCompileOptions & kPTCompileOptionForceTextureLOD0) != 0)
    {
        cameraRay.rayCone.spreadAngle = 0.0f;
    }
    return cameraRay;
}

float3 VisualizeTextureLOD(HitPayload payload, float2 uvDX, float2 uvDY)
{
    GSInstance inst = LoadSceneInstance(payload.instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);
    if (material.baseColorTexture == ~0u)
        return float3(0.0f);

    if ((ptCompileOptions & kPTCompileOptionForceTextureLOD0) != 0)
        return VisualizeApproximateMipLevel(0.0f);

    uint width, height;
    textures[NonUniformResourceIndex(material.baseColorTexture)].GetDimensions(width, height);
    float mipLevel = ApproximateMipLevelFromUVGradient(uvDX, uvDY, width, height);
    return VisualizeApproximateMipLevel(mipLevel);
}

// -- Main Shader Routines

uint SERHashCombine(uint hash, uint value)
{
    return hash * 31u + value;
}

uint GetSERMaterialFamily(GSMaterial material, bool isCurve)
{
    if (material.shaderBlockID == kMaterialShaderBlockHair)
        return 4u;

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
        material.anisotropyTexture != ~0u ||
        material.clearcoatFactor > EPS ||
        material.clearcoatTexture != ~0u)
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
        material.specularColorTexture != ~0u ||
        material.clearcoatTexture != ~0u ||
        material.clearcoatRoughnessTexture != ~0u)
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
    hash = SERHashCombine(hash, material.shaderBlockID);
    hash = SERHashCombine(hash, GetSERMaterialFamily(material, isCurve));
    hash = SERHashCombine(hash, GetSERMaterialFeatureFlags(material));
    hash = SERHashCombine(hash, min(bounce, 3u));
    hash = SERHashCombine(hash, materialIndex);
    return hash & 0x3Fu;
}

uint BuildSERCoherenceHint(HitPayload payload, uint bounce)
{
    if (payload.instance == INSTANCE_NO_HIT)
        return 0u;

    uint instanceID = payload.instance;
    if (IsLightInstance(instanceID))
    {
        GSLight light = LoadSceneLight(GetLightIndex(instanceID));
        uint hash = SERHashCombine(17u, GSLightType(light));
        hash = SERHashCombine(hash, min(bounce, 3u));
        return (1u << 6) | (hash & 0x3Fu);
    }

    bool isCurve = IsCurveInstance(instanceID);
    uint materialIndex;
    if (isCurve)
    {
        GSInstance inst = LoadSceneInstance(instanceID);
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

bool TraceShadowRay<S : ISampler>(float3 origin, float3 direction, float tMax, inout S rng)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = EPS_TMin;
    ray.TMax = tMax;

    RayQuery<RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(AS, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, kMeshMask | kCurveMask, ray);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instance = q.CandidateInstanceID();
            if (IsLightInstance(instance))
                continue;
            if (IsCurveInstance(instance))
            {
                q.CommitNonOpaqueTriangleHit();
                continue;
            }

            uint primitive = q.CandidatePrimitiveIndex();
            float2 bary = q.CandidateTriangleBarycentrics();
            
            GSInstance inst = LoadSceneInstance(instance);
            GSMaterial material = LoadMaterial(inst.materialIndex);
            
            float opacity = 1.0f;
            if (material.baseColorTexture != ~0u) {
                float2 uv = GetHitUV(inst.resourceOffset, primitive, bary, inst);
                float4 baseColor = material.baseColorFactor * textures[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(textureSampler, uv, 0);
                opacity = baseColor.a;
            } else {
                opacity = material.baseColorFactor.a;
            }

            if (opacity == 1.0f || rng.sample() >= 1.0f - opacity) {
                q.CommitNonOpaqueTriangleHit();
            }
        }
    }
    return q.CommittedStatus() != 0;
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
    uint2 pix,
    HitState surfaceHit,
    uint32_t sourceInstance,
    IBSSRDF bssrdf,
    inout S rng,
    out BSSRDFSurfaceSample result)
{
    result = BSSRDFSurfaceSample();

    BSSRDFSurfaceInteraction poBSSRDF = MakeBSSRDFPo(surfaceHit);
    BSSRDFSampleRaySegment segment =
        bssrdf.Sample_SpRay(poBSSRDF, rng.sample(), rng.sample(), rng.sample(), rng.sample());
    if (!segment.valid)
        return false;

    RayDesc queryRay;
    queryRay.Origin = segment.origin;
    queryRay.Direction = segment.direction;
    queryRay.TMin = 0.0f;
    queryRay.TMax = segment.tMax;

    WeightedReservoir<BSSRDFSurfaceCandidate> reservoir = WeightedReservoir<BSSRDFSurfaceCandidate>();
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> q;
    q.TraceRayInline(AS, RAY_FLAG_FORCE_NON_OPAQUE, 0x01, queryRay);
    
    GSInstance surfaceInst = LoadSceneInstance(sourceInstance);

    while (q.Proceed())
    {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
        {
            uint instance = q.CandidateInstanceID();
            if (IsLightInstance(instance))
                continue;

            uint primitive = q.CandidatePrimitiveIndex();
            float2 bary = q.CandidateTriangleBarycentrics();

            PCG prng = PCG(pix, globalParams.frameNumber,
                           hash(uint3(instance, primitive, reservoir.sampleCount)));

            GSInstance inst = LoadSceneInstance(instance);
            
            if (instance == sourceInstance && inst.materialIndex == surfaceInst.materialIndex)
            {
                GSMaterial material = LoadMaterial(inst.materialIndex);
                float opacity = 1.0f;
                if (material.baseColorTexture != ~0u) {
                    float2 uv = GetHitUV(inst.resourceOffset, primitive, bary, inst);
                    float4 baseColor = material.baseColorFactor * textures[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(textureSampler, uv, 0);
                    opacity = baseColor.a;
                } else {
                    opacity = material.baseColorFactor.a;
                }

                if (opacity == 1.0f || prng.sample() >= 1.0f - opacity) {
                    BSSRDFSurfaceCandidate candidate = BSSRDFSurfaceCandidate(instance, primitive, bary, q.CandidateTriangleRayT());
                    reservoir.Add(candidate, 1.0f, prng.sample());
                }
            }
        }
    }

    if (!reservoir.HasSample())
        return false;

    BSSRDFSurfaceCandidate selected = reservoir.selected;
    
    GSInstance piInst = LoadSceneInstance(selected.instance);
    HitState piHit = GetHitState(piInst.resourceOffset, selected.primitive, selected.bary, piInst, queryRay.Origin, queryRay.Direction, selected.hitT, 0.0f);

    float selectionPdf = reservoir.SelectionPDF();
    BSSRDFSurfaceInteraction piBSSRDF = MakeBSSRDFPi(piHit);
    float pdfSp = bssrdf.Pdf_Sp(poBSSRDF, piBSSRDF) * selectionPdf;
    float3 Sp = bssrdf.Sp(poBSSRDF, piBSSRDF);
    if (pdfSp <= EPS || MaxComponent(Sp) <= EPS)
        return false;

    result.valid = true;
    result.surface = piHit;
    result.Sp = Sp;
    result.pdf = pdfSp;
    return true;
}

InfiniteLight MakeEnvironmentLight(GSLight gl)
{
    return InfiniteLight(
        SampledSpectrum(gl.color * gl.power),
        GSLightHasFlag(gl, kGSLightFlagEnvironmentMap),
        gl.params.x,
        globalParams.envMapTextureIndex,
        globalParams.envMapMarginalCDFIndex,
        globalParams.envMapConditionalCDFIndex
    );
}

LightLiSample SampleSceneLight(GSLight gl, LightSampleContext ctx, float2 u)
{
    SampledSpectrum Lemit = SampledSpectrum(gl.color * gl.power);
    switch (GSLightType(gl)) {
    case kGSLightTypeEnvironment: {
        InfiniteLight light = MakeEnvironmentLight(gl);
        return light.Sample_Li(ctx, u);
    }
    case kGSLightTypeDirectional: {
        DistantLight light = DistantLight(Lemit, gl.direction, gl.params.x);
        return light.Sample_Li(ctx, u);
    }
    case kGSLightTypePoint: {
        PointLight light = PointLight(Lemit, gl.position, gl.params.x);
        return light.Sample_Li(ctx, u);
    }
    case kGSLightTypeSpot: {
        SpotLight light = SpotLight(Lemit, gl.position, gl.direction,
            acos(gl.params.z), acos(gl.params.y), gl.params.x);
        return light.Sample_Li(ctx, u);
    }
    case kGSLightTypeDisk: {
        DiskLight light = DiskLight(Lemit, gl.position, gl.dpdu, gl.dpdv, gl.params.xy, GSLightHasFlag(gl, kGSLightFlagTwoSided));
        return light.Sample_Li(ctx, u);
    }
    case kGSLightTypeRect: {
        RectLight light = RectLight(Lemit, gl.position,
            gl.dpdu, gl.dpdv, GSLightHasFlag(gl, kGSLightFlagTwoSided));
        return light.Sample_Li(ctx, u);
    }
    default:
        return LightLiSample();
    }
}

LightLiSample SampleSceneLight(LightSampleContext ctx, float uc, float2 u)
{
    uint lightIndex;
    float selectionPMF;
    if (!SampleLightSelection(ctx, uc, lightIndex, selectionPMF) || selectionPMF <= 0.0f)
        return LightLiSample();

    GSLight gl = LoadSceneLight(lightIndex);
    LightLiSample Li = SampleSceneLight(gl, ctx, u);
    if (Li.pdf > 0.0f)
    {
        SampledSpectrum radiance = Li.Li / (Li.pdf * selectionPMF);
        float mixPDF = Li.isDelta ? Li.pdf : (Li.pdf * selectionPMF);
        return LightLiSample(radiance, Li.wi, mixPDF, Li.isDelta, Li.dist, GSLightHasFlag(gl, kGSLightFlagUseShadow));
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
    bool upperHemisphere,
    inout S rng,
    out LightSampleContext ctx,
    out LightLiSample Li)
{
    ctx = LightSampleContext(hit.position, hit.geometryNormal, hit.normal, upperHemisphere);
    Li = SampleSceneLight(ctx, rng.sample(), rng.sample2D());
    if (Li.pdf <= EPS)
        return false;

    float3 shadowNormalFF = dot(Li.wi, hit.geometryNormal) < 0.0f ? -hit.geometryNormal : hit.geometryNormal;
    float3 shadowOrigin = OffsetRay(hit.position, shadowNormalFF);
    if (Li.useShadow && TraceShadowRay(shadowOrigin, Li.wi, Li.dist, rng))
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
    HitState hit,
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
    if (!SampleVisibleSceneLight(hit, !bsdf.HasTransmission(), rng, ctx, Li))
    {
        lighting.lastLightCtx = ctx;
        return;
    }

    lighting.lastLightCtx = ctx;
    float3 wi = WorldToShadingLocal(hit, Li.wi);
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
    if (!SampleVisibleSceneLight(hit, true, rng, ctx, Li))
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
    IMaterial material,
    float3 wo,
    float3 beta,
    uint bounce,
    AOVCategory firstBounceCategory,
    inout PathRadiance radiance)
{
    AddRadianceByCategory(radiance, beta * material.Le(wo),
                          bounce, firstBounceCategory, AOVCategory::Diffuse, bounce == 0);
}

void WritePrimaryInstanceID(uint2 pix, uint32_t instanceID)
{
    instanceIDBuffer[pix] = instanceID;
}

void AccumulatePathRadiance(uint2 pix, PathRadiance radiance, float ndcDepth, float sampleCount)
{
    float4 outD = float4(radiance.diffuse, sampleCount);
    float4 outS = float4(radiance.specular, 1.0f);

    if (sampleCount == 1) {
        diffuse[pix]  = outD;
        specular[pix] = outS;
        depth[pix] = ndcDepth;
    } else {
        float invN = 1.0f / sampleCount;
        float4 currentD = diffuse[pix];
        currentD.rgb += (outD.rgb - currentD.rgb) * invN;
        currentD.a = sampleCount;
        diffuse[pix] = currentD;
        
        specular[pix] += (outS - specular[pix]) * invN;
        depth[pix] += (ndcDepth - depth[pix]) * invN;
    }

    uint frameA = (uint(sampleCount) + 1) / 2;
    float3 totalRadiance = outD.rgb + outS.rgb;
    if (uint(sampleCount) % 2 == 1)
    {
        float3 currentA = adaptiveAux[pix].xyz;
        if (frameA == 1) {
            adaptiveAux[pix] = float4(totalRadiance, 0.0f);
        } else {
            float invNA = 1.0f / float(frameA);
            adaptiveAux[pix] = float4(currentA + (totalRadiance - currentA) * invNA, adaptiveAux[pix].w);
        }
    }

    if (sampleCount > globalParams.adaptiveMinSamples && globalParams.adaptiveThreshold > 0.0f)
    {
        float3 I = diffuse[pix].rgb + specular[pix].rgb;
        float3 A = adaptiveAux[pix].xyz;
        
        float error_difference = abs(I.r - A.r) + abs(I.g - A.g) + abs(I.b - A.b);
        float intensity = I.r + I.g + I.b;
        
        float error_normalize = intensity < 1.0f ? sqrt(intensity) : intensity;
        float error = error_difference / (0.0001f + error_normalize);
        
        bool did_converge = (error < globalParams.adaptiveThreshold);
        adaptiveAux[pix] = float4(adaptiveAux[pix].xyz, did_converge ? 1.0f : 0.0f);
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

    if (hit.kind == HitKind::Point)
    {
        PointLight light = PointLight(Lemit, gl.position, gl.params.x);
        float3 normal = normalize(hitPoint - gl.position);
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, rayDirection, true);
    }
    else if (hit.kind == HitKind::Spot)
    {
        SpotLight light = SpotLight(Lemit, gl.position, gl.direction,
            acos(gl.params.z), acos(gl.params.y), gl.params.x);
        float3 normal = normalize(hitPoint - gl.position);
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, rayDirection, true);
    }
    else if (hit.kind == HitKind::Disk)
    {
        DiskLight light = DiskLight(Lemit, gl.position, gl.dpdu, gl.dpdv, gl.params.xy, GSLightHasFlag(gl, kGSLightFlagTwoSided));
        float3 normal = normalize(cross(gl.dpdu, gl.dpdv));
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, rayDirection, true);
    }
    else if (hit.kind == HitKind::Rect)
    {
        RectLight light = RectLight(Lemit, gl.position, gl.dpdu, gl.dpdv, GSLightHasFlag(gl, kGSLightFlagTwoSided));
        float3 normal = normalize(cross(gl.dpdu, gl.dpdv));
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, rayDirection, true);
    }

    bool allowPrimary = globalParams.ptPrimaryLightVisibility != 0u;
    if ((bounce > 0 || allowPrimary) && any(Le > 0.0f))
    {
        float pLightSelection = EvalLightSelectionPMF(lighting.lastLightCtx, hit.lightIndex);
        float pdf = pLightSelection * pdfLi;
        float mis = lastSamplePDF > EPS ? BalanceHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
        float3 contrib = beta * Le * mis;
        bool direct = bounce == 0;
        AddRadianceByCategory(radiance, contrib, bounce, firstBounceCategory, AOVCategory::Specular, direct);
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
    float3 wi = -view;
    float3 Le = lighting.env.Le(wi);
    float pdf = EvalLightSelectionPMF(lighting.lastLightCtx, 0u) *
        lighting.env.PDF_Li(lighting.lastLightCtx, wi, true);
    // Sun disks follow the environment light.
    for (uint i = 1u; i < globalParams.lights.count; ++i)
    {
        GSLight gl = LoadSceneLight(i);
        if (GSLightType(gl) != kGSLightTypeDirectional || gl.params.x <= 0.0f)
            break;
        DistantLight sun = DistantLight(SampledSpectrum(gl.color * gl.power), gl.direction, gl.params.x);
        float3 sunLe = sun.Le(wi);
        if (!any(sunLe > 0.0f))
            continue;

        float pSel = EvalLightSelectionPMF(lighting.lastLightCtx, i);
        pdf += pSel * sun.PDF_Li(lighting.lastLightCtx, wi, true);
        if (bounce > 0 || globalParams.ptPrimaryLightVisibility != 0u)
            Le += sunLe;
    }

    if (!any(Le > 0.0f))
        return;

    if (bounce == 0)
    {
        AddRadiance(radiance.specular, Le, true);
        return;
    }

    float mis = lastSamplePDF > EPS ? PowerHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
    float3 contrib = beta * Le * mis;
    AddRadianceByCategory(radiance, contrib, bounce, firstBounceCategory, AOVCategory::Specular, false);
}

void RayGenerationImpl(uint2 pix)
{
    if (pix.x >= uint(globalParams.fbWidth) || pix.y >= uint(globalParams.fbHeight))
        return;

    if (globalParams.ptAccumulatedFrames > globalParams.adaptiveMinSamples &&
        globalParams.adaptiveThreshold > 0.0f &&
        adaptiveAux[pix].w != 0.0f)
    {
        return;
    }

    for (uint subSample = 0; subSample < globalParams.ptSamplesPerPixel; subSample++)
    {
        uint sampleIndex = globalParams.ptAccumulatedFrames + subSample;

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

#ifdef USE_SER
[shader("raygeneration")]
void RayGeneration()
{
    RayGenerationImpl(DispatchRaysIndex().xy);
}
#else
[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 tid : SV_DispatchThreadID)
{
    RayGenerationImpl(tid.xy);
}
#endif

void PathTrace<S : ISampler>(uint2 pix, uint subSample, inout S rng)
{
    float3 beta = float3(1);
    PathRadiance radiance = PathRadiance();
    AOVCategory firstBounceCategory = AOVCategory::Undefined;
    bool pickingWritten = false;
    bool resetAccumulation = globalParams.ptAccumulatedFrames == 0u && subSample == 0u;
    float sampleCount = resetAccumulation ? 1.0f : diffuse[pix].a + 1.0f;
    float lastSamplePDF = 0.0f;
    // Serialized scenes keep the environment light as the first scene light.
    GSLight envLight = LoadSceneLight(0u);
    InfiniteLight env = MakeEnvironmentLight(envLight);
    DirectLightingState lighting = DirectLightingState(env);
    uint diffuseBounces = 0;
    uint specularBounces = 0;
    uint transmissionBounces = 0;
    const int supportDebugViewFlags = kViewBaseColor | kViewNormal | kViewPosition | kViewTextureLOD;
    bool hasAnyDebugView = (globalParams.dbgViewFlags & supportDebugViewFlags) != 0u;
    CameraRay cameraRay = GenerateCameraRay(pix, rng, hasAnyDebugView);
    RayDesc ray = cameraRay.ray;
    RayCone currentRayCone = cameraRay.rayCone;
    float chitNDCDepth = 0.0f;
    for (uint bounce = 0; ; bounce++) {
        HitPayload payload;

        RayQuery<RAY_FLAG_NONE> q;
        q.TraceRayInline(AS, RAY_FLAG_NONE, 0xFF, ray);
        float2 bestProceduralBary = float2(0.0f);
        while (q.Proceed())
        {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE)
            {
                uint instance = q.CandidateInstanceID();
                if (IsCurveInstance(instance))
                {
                    q.CommitNonOpaqueTriangleHit();
                    continue;
                }
                uint primitive = q.CandidatePrimitiveIndex();
                float2 bary = q.CandidateTriangleBarycentrics();
                if (!EvaluateOpacityAlphaTest(instance, primitive, bary, rng))
                {
                    q.CommitNonOpaqueTriangleHit();
                }
            }
            else if (IsLightInstance(q.CandidateInstanceID()))
            {
                uint instance = q.CandidateInstanceID();
                GSLight gl = LoadSceneLight(GetLightIndex(instance));
                float3 ro = q.CandidateObjectRayOrigin();
                float3 rd = q.CandidateObjectRayDirection();
                float t;
                float2 localPos;
                if (EvaluateLightIntersection(GSLightType(gl), ro, rd, q.RayTMin(), q.CommittedRayT(), t, localPos))
                {
                    bestProceduralBary = localPos;
                    q.CommitProceduralPrimitiveHit(t);
                }
            }
        }

        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            payload.instance = q.CommittedInstanceID();
            payload.primitive = q.CommittedPrimitiveIndex();
            payload.bary = q.CommittedTriangleBarycentrics();
            payload.hitT = q.CommittedRayT();
        }
        else if (q.CommittedStatus() == COMMITTED_PROCEDURAL_PRIMITIVE_HIT)
        {
            payload.instance = q.CommittedInstanceID();
            payload.primitive = q.CommittedPrimitiveIndex();
            payload.bary = bestProceduralBary;
            payload.hitT = q.CommittedRayT();
        }
        else
        {
            payload.instance = INSTANCE_NO_HIT;
        }

        #ifdef USE_SER
            uint coherenceHint = BuildSERCoherenceHint(payload, bounce);
            ReorderThread(coherenceHint, kSERCoherenceHintBits);
        #endif

        // Outgoing direction
        float3 view = -ray.Direction;
        HitRecord hit = MakeHitRecord(payload, ray);

        float3 hitPoint;
        if (!hasAnyDebugView && AddAnalyticLightHitEmission(hit, ray.Direction, view, beta, bounce, firstBounceCategory,
                                        lastSamplePDF, lighting, radiance, hitPoint))
        {
            ray.Origin = hitPoint + ray.Direction * EPS_TMin;
            bounce--; // Don't count this as a bounce
            continue;
        }

        if (!hasAnyDebugView && hit.kind == HitKind::None)
        {
            AddEnvironmentMiss(view, beta, bounce, firstBounceCategory, lastSamplePDF, lighting, radiance);
            break;
        }
        if (!IsMaterialSurfaceHit(hit.kind))
            break;

        currentRayCone.width += currentRayCone.spreadAngle * hit.raw.hitT;

        HitState hitState;
        if (hit.kind == HitKind::Curve)
        {
            GSInstance inst = LoadSceneInstance(hit.raw.instance);
            hitState = GetCurveHitState(inst.resourceOffset, hit.raw.primitive, hit.raw.bary, inst,
                               ray.Origin, ray.Direction, hit.raw.hitT);
        }
        else
        {
            GSInstance inst = LoadSceneInstance(hit.raw.instance);
            hitState = GetHitState(inst.resourceOffset, hit.raw.primitive, hit.raw.bary, inst, ray.Origin, ray.Direction, hit.raw.hitT, currentRayCone.width);
        }

        float3 wo = WorldToShadingLocal(hitState, view);
        bool foldSubsurfaceToDiffuse = diffuseBounces > 0;
        float3 baseColorOut;
        IMaterial material = MakeMaterial(hit.raw, hitState, wo, baseColorOut, hit.kind == HitKind::Curve, foldSubsurfaceToDiffuse);

        if (hasAnyDebugView && bounce == 0)
        {
            specular[pix] = diffuse[pix] = float4(0.0f);
            int flag = globalParams.dbgViewFlags;
            if ((flag & kViewBaseColor) != 0)
                diffuse[pix] = float4(baseColorOut, 1.0f);
            if ((flag & kViewNormal) != 0)
                diffuse[pix] = float4(hitState.normal * 0.5f + 0.5f, 1.0f);

            if ((flag & kViewPosition) != 0)
                diffuse[pix] = float4(hitState.position, 1.0f);
            if ((flag & kViewTextureLOD) != 0)
                diffuse[pix] = float4(VisualizeTextureLOD(hit.raw, hitState.uvDX, hitState.uvDY), 1.0f);            
            // ...
            WritePrimaryInstanceID(pix, hit.raw.instance);
            return;
        }

        if (bounce == 0 && !pickingWritten)
        {
            WritePrimaryInstanceID(pix, hit.raw.instance);
            pickingWritten = true;
        }

        if (bounce == 0)
            chitNDCDepth = WorldToNDCDepth(hitState.position);

        // Local frame setup
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
            if (!SeparableBSSRDF_SampleSp(pix, hitState, hit.raw.instance, bssrdf, rng, sample))
                break;

            float3 localBeta = beta * sample.Sp / max(pSampleSubsurface * sample.pdf, EPS);
            if (any(isnan(localBeta)) || any(isinf(localBeta)))
                break;

            HitState exitSurface = sample.surface;
            // Lambertian assumed for subsurface interactions
            const float3 exitF = float3(InvPi);
            AddDiffuseDirectLighting(exitSurface, exitF, localBeta,
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
            nextSample.hit = exitSurface;
        }
        else
        {
            float3 localBeta = beta / pSampleSurface;
            IBSDF bsdf = material.GetSurface();
            // Dirac surfaces do not get direct light contribution
            if (!bsdf.IsDirac())
                AddLocalDirectLighting(hitState, bsdf, wo, localBeta,
                                   bounce, firstBounceCategory, rng, lighting, radiance);
            AddSurfaceEmissive(material, wo, localBeta, bounce, firstBounceCategory, radiance);

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
            nextSample.hit = hitState;
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
            float rrPCount = min(dot(beta, float3(0.2126f, 0.7152f, 0.0722f)) + 0.01f, 0.95f);
            if (rng.sample() > rrPCount) // Contribution is low, may terminate early
                break;
            beta /= rrPCount; // Boost non-terminated paths
        }
    }
    if (!pickingWritten)
        WritePrimaryInstanceID(pix, ~0u);

    AccumulatePathRadiance(pix, radiance, chitNDCDepth, sampleCount);
}

bool EvaluateOpacityAlphaTest<S : ISampler>(uint instance, uint primitive, float2 bary, inout S rng)
{
    if (IsLightInstance(instance)) return false;
    GSInstance inst = LoadSceneInstance(instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);
    
    float opacity = material.baseColorFactor.a;
    if (material.baseColorTexture != ~0u) {
        float2 uv = GetHitUV(inst.resourceOffset, primitive, bary, inst);
        opacity *= textures[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(textureSampler, uv, 0).a;
    }
    
    if (opacity == 1.0f) return false; // opaque, don't ignore
    return rng.sample() < 1.0f - opacity; // true if we should ignore the hit
}

bool EvaluateLightIntersection(uint lightType, float3 ro, float3 rd, float rayTMin, float rayTMax, out float hitT, out float2 localPosition)
{
    hitT = 0.0f;
    localPosition = float2(0.0f);
    if (lightType == kGSLightTypePoint || lightType == kGSLightTypeSpot)
    {
        float a = dot(rd, rd);
        float b = dot(ro, rd);
        float c = dot(ro, ro) - 1.0f;
        float discriminant = b * b - a * c;
        if (a <= 1e-12f || discriminant < 0.0f)
            return false;

        float root = sqrt(discriminant);
        float t = (-b - root) / a;
        if (t < rayTMin || t > rayTMax)
        {
            t = (-b + root) / a;
            if (t < rayTMin || t > rayTMax)
                return false;
        }

        float3 p = ro + rd * t;
        hitT = t;
        localPosition = p.xy;
        return true;
    }

    if (abs(rd.z) < 1e-7f)
        return false;

    float t = -ro.z / rd.z;
    if (t < rayTMin || t > rayTMax)
        return false;

    float2 p = (ro + rd * t).xy;
    if (lightType == kGSLightTypeDisk)
    {
        if (dot(p, p) > 1.0f) return false;
    }
    else if (lightType == kGSLightTypeRect)
    {
        if (any(abs(p) > 1.0f)) return false;
    }
    else return false;

    hitT = t;
    localPosition = p;
    return true;
}
