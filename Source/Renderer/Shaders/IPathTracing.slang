import IBindless;
import IFlags;
import ICommon;
import ILight;
import IBSDF;
import IBSSRDF;
import IMaterial;
import IMath;
import IBxDF;

#ifndef SHARC_UPDATE
#define SHARC_UPDATE 0
#endif

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

[[SpecializationConstant]] uint kCompileOptions;

uniform UBO globalParams;
// Scene
RaytracingAccelerationStructure TLAS;
StructuredBuffer<GSInstance> gInstances;
// Primitives, either static or dynamic
ByteAddressBuffer gPrimBuffer;
ByteAddressBuffer gDynamicPrimBuffer;
StructuredBuffer<GSMaterial> gMaterials;
// Lights
// GPUScene sorts these by type (env, directional, ...see kGSLightType*)
// gLights[0] always exists and is the environment light.
StructuredBuffer<GSLight> gLights;
StructuredBuffer<GSEmissiveCluster> gEmissiveClusters;
#include "ILightBVH.slang"
SamplerState gTexSampler;

#if !SHARC_UPDATE
// Path radiance accumulation buffers
RWTexture2D<float4> diffuse;            // Diffuse lobe radiance
RWTexture2D<float4> specular;           // Specular lobe radiance (incl. transmission)
RWTexture2D<float> depth;               // NDC depth
// Auxilary buffers
RWTexture2D<uint32_t> instanceIDBuffer;
#endif

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
    return gInstances[NonUniformResourceIndex(GetInstanceBufferIndex(localInstanceIndex))];
}

bool IsCurveInstance(uint instance)
{
    return LoadSceneInstance(instance).type == kGSInstanceTypeCurve;
}

GSMaterial LoadMaterial(uint localMaterialIndex)
{
    return gMaterials[NonUniformResourceIndex(GetMaterialBufferIndex(localMaterialIndex))];
}

GSLight LoadSceneLight(uint localLightIndex)
{
    return gLights[NonUniformResourceIndex(globalParams.lights.offset + localLightIndex)];
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

float4 SampleMaterialTexture(uint textureIndex, float2 uv, float2 uvDX, float2 uvDY)
{
    if ((kCompileOptions & uint32_t(PTCompileOptionsBits.ForceTextureLOD0)) != 0)
        return gTextures2D[NonUniformResourceIndex(textureIndex)].SampleLevel(gTexSampler, uv, 0);
    return gTextures2D[NonUniformResourceIndex(textureIndex)].SampleGrad(gTexSampler, uv, uvDX, uvDY);
}

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

HitState GetHitState(uint meshOffset, uint primitive, float2 bary, GSInstance inst, float3 rayOrigin, float3 rayDirection, float hitT, float rayConeWidth)
{
    if (GSInstanceIsDynamic(inst))
        return GetHitState(gDynamicPrimBuffer, meshOffset, primitive, bary, inst, rayOrigin, rayDirection, hitT, rayConeWidth);
    return GetHitState(gPrimBuffer, meshOffset, primitive, bary, inst, rayOrigin, rayDirection, hitT, rayConeWidth);
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
        return GetHitUV(gDynamicPrimBuffer, meshOffset, primitive, bary);
    return GetHitUV(gPrimBuffer, meshOffset, primitive, bary);
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

FCurveLeaf LoadCurveLeaf(GSCurveSet curve, uint primitive)
{
    uint leafIndex = primitive / 4u;
    return gPrimBuffer.Load<FCurveLeaf>(curve.leaves.offset + leafIndex * sizeof(FCurveLeaf));
}

HitState GetCurveHitState(uint curveOffset, uint primitive, float2 /*triangleBary*/, GSInstance inst,
                          float3 rayOrigin, float3 rayDirection, float hitT)
{
    HitState hit;
    hit.uvDX = float2(0.0f);
    hit.uvDY = float2(0.0f);

    GSCurveSet curve = gPrimBuffer.Load<GSCurveSet>(curveOffset);
    FCurveLeaf leaf = LoadCurveLeaf(curve, primitive);

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

bool IsPanoramicCamera()
{
    return globalParams.cameraProjection == uint32_t(CameraProjection::Panoramic);
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

// TODO UBO should supply jitter instead
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
    return saturate(clipPosition.z / clipPosition.w);
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

    if ((kCompileOptions & uint32_t(PTCompileOptionsBits.ForceTextureLOD0)) != 0)
    {
        cameraRay.rayCone.spreadAngle = 0.0f;
    }
    return cameraRay;
}

// SER Helpers
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
    q.TraceRayInline(TLAS, RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, kMeshMask | kCurveMask, ray);

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
                float4 baseColor = material.baseColorFactor * gTextures2D[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(gTexSampler, uv, 0);
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

InfiniteLight MakeEnvironmentLight(GSLight gl)
{
    return InfiniteLight(
        SampledSpectrum(gl.color * gl.power),
        GSLightHasFlag(gl, uint32_t(GSLightFlagsBits.EnvironmentMap)),
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
        DiskLight light = DiskLight(Lemit, gl.position, gl.dpdu, gl.dpdv, gl.params.xy,
                                    GSLightHasFlag(gl, uint32_t(GSLightFlagsBits.TwoSided)));
        return light.Sample_Li(ctx, u);
    }
    case kGSLightTypeRect: {
        RectLight light = RectLight(Lemit, gl.position,
            gl.dpdu, gl.dpdv, GSLightHasFlag(gl, uint32_t(GSLightFlagsBits.TwoSided)));
        return light.Sample_Li(ctx, u);
    }
    default:
        return LightLiSample();
    }
}

uint SampleAliasTable(uint byteOffset, uint count, float u, out float pdf)
{
    uint binIndex = min(uint(u * float(count)), count - 1u);
    float localU = u * float(count) - float(binIndex);
    GSAlias bin = gPrimBuffer.Load<GSAlias>(byteOffset + binIndex * sizeof(GSAlias));
    uint selected = localU >= bin.select ? min(bin.alias, count - 1u) : binIndex;
    pdf = gPrimBuffer.Load<GSAlias>(byteOffset + selected * sizeof(GSAlias)).prob;
    return selected;
}

void LoadEmissiveClusterTriangle(GSEmissiveCluster cluster, uint triangleIndex,
                                 out float3 p0, out float3 p1, out float3 p2,
                                 out float2 uv0, out float2 uv1, out float2 uv2)
{
    GSInstance inst = LoadSceneInstance(cluster.instanceIndex);
    GSMesh mesh = gPrimBuffer.Load<GSMesh>(inst.resourceOffset);
    FMeshlet meshlet =
        gPrimBuffer.Load<FMeshlet>(mesh.meshlets.offset + cluster.meshletIndex * sizeof(FMeshlet));
    uint triBase = mesh.meshletTriOffset + meshlet.triOffset + triangleIndex * sizeof(MIndex);
    MIndex tri = gPrimBuffer.Load<MIndex>(triBase);
    uint vtxBase = mesh.meshletVtxOffset + meshlet.vtxOffset * sizeof(uint32_t);
    uint i0 = gPrimBuffer.Load<uint32_t>(vtxBase + uint(tri.v0) * sizeof(uint32_t));
    uint i1 = gPrimBuffer.Load<uint32_t>(vtxBase + uint(tri.v1) * sizeof(uint32_t));
    uint i2 = gPrimBuffer.Load<uint32_t>(vtxBase + uint(tri.v2) * sizeof(uint32_t));
    float3 n, t;
    float sign;
    FQDecode(gPrimBuffer.Load<FQVertex>(mesh.vertices.offset + i0 * sizeof(FQVertex)), p0, n, t, sign, uv0);
    FQDecode(gPrimBuffer.Load<FQVertex>(mesh.vertices.offset + i1 * sizeof(FQVertex)), p1, n, t, sign, uv1);
    FQDecode(gPrimBuffer.Load<FQVertex>(mesh.vertices.offset + i2 * sizeof(FQVertex)), p2, n, t, sign, uv2);
    p0 = inst.rotation.rotate(p0) * inst.scale + inst.transform;
    p1 = inst.rotation.rotate(p1) * inst.scale + inst.transform;
    p2 = inst.rotation.rotate(p2) * inst.scale + inst.transform;
}

float3 EvaluateClusterEmission(uint instanceIndex, float2 uv, float3 wo)
{
    GSInstance inst = LoadSceneInstance(instanceIndex);
    GSMaterial material = LoadMaterial(inst.materialIndex);
    float3 emission = material.emissiveFactor;
    if (material.emissiveTexture != ~0u)
        emission *= gTextures2D[NonUniformResourceIndex(material.emissiveTexture)]
            .SampleLevel(gTexSampler, uv, 0.0f).rgb;
    float clearcoat = material.clearcoatFactor;
    if (material.clearcoatTexture != ~0u)
        clearcoat *= gTextures2D[NonUniformResourceIndex(material.clearcoatTexture)]
            .SampleLevel(gTexSampler, uv, 0.0f).r;
    float clearcoatRoughness = material.clearcoatRoughnessFactor;
    if (material.clearcoatRoughnessTexture != ~0u)
        clearcoatRoughness *= gTextures2D[NonUniformResourceIndex(material.clearcoatRoughnessTexture)]
            .SampleLevel(gTexSampler, uv, 0.0f).g;
    return emission * CoatEscape(saturate(clearcoat), saturate(clearcoatRoughness), wo,
                                 globalParams.ggxLutEIndex, globalParams.ggxLutEavgIndex,
                                 globalParams.energyCompensation != 0u);
}

LightLiSample SampleEmissiveCluster(uint clusterIndex, LightSampleContext ctx, float aliasU, float2 u)
{
    GSEmissiveCluster cluster = GetEmissiveCluster(clusterIndex);
    if (cluster.triCount == 0u)
        return LightLiSample();
    float trianglePMF;
    uint triangleIndex = SampleAliasTable(cluster.aliasOffset, cluster.triCount, aliasU, trianglePMF);
    if (trianglePMF <= 0.0f)
        return LightLiSample();

    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
    LoadEmissiveClusterTriangle(cluster, triangleIndex, p0, p1, p2, uv0, uv1, uv2);
    float sqrtU = sqrt(u.x);
    float b1 = sqrtU * (1.0f - u.y);
    float b2 = sqrtU * u.y;
    float2 uv = uv0 + b1 * (uv1 - uv0) + b2 * (uv2 - uv0);
    TriangleLight light = TriangleLight(float3(1.0f), p0, p1 - p0, p2 - p0, true);
    LightLiSample Li = light.Sample_Li(ctx, u);
    float3 wo = float3(0.0f, 0.0f, dot(light.normal, -Li.wi));
    Li.Li = EvaluateClusterEmission(cluster.instanceIndex, uv, wo);
    Li.pdf *= trianglePMF;
    return Li;
}

LightLiSample SampleSceneLight(LightSampleContext ctx, float uc, float aliasU, float2 u)
{
    uint lightIndex;
    float selectionPMF;
    if (!SampleLightSelection(ctx, uc, lightIndex, selectionPMF, (kCompileOptions & uint32_t(PTCompileOptionsBits.LightSamplerUniform)) != 0u))
        return LightLiSample();

    bool isCluster = IsClusterSelection(lightIndex);
    GSLight gl = {};
    LightLiSample Li;
    if (isCluster)
        Li = SampleEmissiveCluster(SelectionIndex(lightIndex), ctx, aliasU, u);
    else
    {
        gl = LoadSceneLight(lightIndex);
        Li = SampleSceneLight(gl, ctx, u);
    }
    if (Li.pdf > 0.0f)
    {
        SampledSpectrum radiance = Li.Li / (Li.pdf * selectionPMF);
        float mixPDF = Li.isDelta ? Li.pdf : (Li.pdf * selectionPMF);
        return LightLiSample(radiance, Li.wi, mixPDF, Li.isDelta, Li.dist,
                             isCluster || GSLightHasFlag(gl, uint32_t(GSLightFlagsBits.UseShadow)));
    }
    return LightLiSample();
}

bool SampleVisibleSceneLight<S : ISampler>(
    HitState hit,
    bool upperHemisphere,
    inout S rng,
    out LightSampleContext ctx,
    out LightLiSample Li)
{
    ctx = LightSampleContext(hit.position, hit.geometryNormal, hit.normal, upperHemisphere);
    Li = SampleSceneLight(ctx, rng.sample(), rng.sample(), rng.sample2D());
    if (Li.pdf <= EPS)
        return false;

    float3 shadowNormalFF = dot(Li.wi, hit.geometryNormal) < 0.0f ? -hit.geometryNormal : hit.geometryNormal;
    float3 shadowOrigin = OffsetRay(hit.position, shadowNormalFF);
    float shadowDistance = Li.dist;
    if (shadowDistance < INF)
        shadowDistance = max(EPS_TMin, shadowDistance - max(EPS_TMin, shadowDistance * 1e-4f));
    if (Li.useShadow && TraceShadowRay(shadowOrigin, Li.wi, shadowDistance, rng))
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

void AddRadiance(inout float3 L, float3 v, bool direct = false)
{
    if (any(isnan(v)) || any(isinf(v)))
        return;
    v = max(float3(0.0f), v);
    if (!direct) {
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

bool AddEmissiveClusterHitEmission(
    HitPayload payload,
    HitState hitState,
    float3 rayDirection,
    float3 beta,
    uint bounce,
    AOVCategory firstBounceCategory,
    float lastSamplePDF,
    DirectLightingState lighting,
    inout PathRadiance radiance)
{
    GSInstance inst = LoadSceneInstance(payload.instance);
    if (inst.emissiveClusterOffset == ~0u || GSInstanceIsDynamic(inst) || GSInstanceType(inst) != kGSInstanceTypeMesh)
        return false;
    GSMesh mesh = gPrimBuffer.Load<GSMesh>(inst.resourceOffset);
    if (payload.primitive >= mesh.emissivePrimitiveMap.count)
        return false;
    uint packed = gPrimBuffer.Load<uint32_t>(mesh.emissivePrimitiveMap.offset + payload.primitive * sizeof(uint32_t));
    if (packed == ~0u)
        return false;
    uint meshletOrdinal = packed >> 8u;
    uint localTriangle = packed & 0xffu;
    uint clusterIndex = inst.emissiveClusterOffset + meshletOrdinal;
    if (clusterIndex >= globalParams.emissiveClusters.count)
        return false;
    GSEmissiveCluster cluster = GetEmissiveCluster(clusterIndex);
    if (localTriangle >= cluster.triCount || cluster.flux <= 0.0f)
        return false;

    float trianglePMF =
        gPrimBuffer.Load<GSAlias>(cluster.aliasOffset + localTriangle * sizeof(GSAlias)).prob;
    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
    LoadEmissiveClusterTriangle(cluster, localTriangle, p0, p1, p2, uv0, uv1, uv2);
    TriangleLight light = TriangleLight(float3(1.0f), p0, p1 - p0, p2 - p0, true);
    float lightPDF = EvalLightSelectionPMF(
        lighting.lastLightCtx, kLightSelectionClusterBit | clusterIndex,
        (kCompileOptions & uint32_t(PTCompileOptionsBits.LightSamplerUniform)) != 0u);
    lightPDF *= trianglePMF * light.PDF_Li(lighting.lastLightCtx, rayDirection, true);
    float mis = (bounce > 0u && lastSamplePDF > EPS)
        ? PowerHeuristic(1, lastSamplePDF, 1, lightPDF)
        : 1.0f;
    float3 emissionWo = float3(0.0f, 0.0f, dot(light.normal, -rayDirection));
    float3 emission = EvaluateClusterEmission(payload.instance, hitState.uv, emissionWo);
    AddRadianceByCategory(radiance, beta * emission * mis,
                          bounce, firstBounceCategory, AOVCategory::Diffuse, bounce == 0u);
    return true;
}

void WritePrimaryInstanceID(uint2 pix, uint32_t instanceID)
{
#if !SHARC_UPDATE
    instanceIDBuffer[pix] = instanceID;
#endif
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
        DiskLight light = DiskLight(Lemit, gl.position, gl.dpdu, gl.dpdv, gl.params.xy,
                                    GSLightHasFlag(gl, uint32_t(GSLightFlagsBits.TwoSided)));
        float3 normal = normalize(cross(gl.dpdu, gl.dpdv));
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, rayDirection, true);
    }
    else if (hit.kind == HitKind::Rect)
    {
        RectLight light =
            RectLight(Lemit, gl.position, gl.dpdu, gl.dpdv, GSLightHasFlag(gl, uint32_t(GSLightFlagsBits.TwoSided)));
        float3 normal = normalize(cross(gl.dpdu, gl.dpdv));
        Le = light.L(hitPoint, normal, float3(0), view);
        pdfLi = light.PDF_Li(lighting.lastLightCtx, rayDirection, true);
    }

    bool allowPrimary = globalParams.ptPrimaryLightVisibility != 0u;
    if ((bounce > 0 || allowPrimary) && any(Le > 0.0f))
    {
        float pLightSelection = EvalLightSelectionPMF(lighting.lastLightCtx, hit.lightIndex, (kCompileOptions & uint32_t(PTCompileOptionsBits.LightSamplerUniform)) != 0u);
        float pdf = pLightSelection * pdfLi;
        float mis = lastSamplePDF > EPS ? BalanceHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
        float3 contrib = beta * Le * mis;
        bool direct = bounce == 0;
        AddRadianceByCategory(radiance, contrib, bounce, firstBounceCategory, AOVCategory::Specular, direct);
    }

    return true;
}

float3 EvaluateInfiniteMissRadiance(float3 wi, InfiniteLight env)
{
    float3 Le = env.Le(wi);
    for (uint lightIndex = 1u; lightIndex < globalParams.lights.count; ++lightIndex)
    {
        GSLight gl = LoadSceneLight(lightIndex);
        if (GSLightType(gl) != kGSLightTypeDirectional)
            break;

        DistantLight light = DistantLight(SampledSpectrum(gl.color * gl.power), gl.direction, gl.params.x);
        Le += light.Le(wi);
    }
    return Le;
}

void AddInfiniteMiss(
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
    float pdf = EvalLightSelectionPMF(lighting.lastLightCtx, 0u, (kCompileOptions & uint32_t(PTCompileOptionsBits.LightSamplerUniform)) != 0u) *
        lighting.env.PDF_Li(lighting.lastLightCtx, wi, true);

    if (any(Le > 0.0f))
    {
        float mis = (lastSamplePDF > EPS && bounce > 0) ? PowerHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
        AddRadianceByCategory(radiance, beta * Le * mis, bounce, firstBounceCategory, AOVCategory::Specular, false);
    }

    if (bounce == 0u && globalParams.ptPrimaryLightVisibility == 0u)
        return;

    for (uint lightIndex = 1u; lightIndex < globalParams.lights.count; ++lightIndex)
    {
        GSLight gl = LoadSceneLight(lightIndex);
        if (GSLightType(gl) != kGSLightTypeDirectional)
            break;

        DistantLight light = DistantLight(SampledSpectrum(gl.color * gl.power), gl.direction, gl.params.x);
        Le = light.Le(wi);
        if (!any(Le > 0.0f))
            continue;

        pdf = EvalLightSelectionPMF(lighting.lastLightCtx, lightIndex, (kCompileOptions & uint32_t(PTCompileOptionsBits.LightSamplerUniform)) != 0u) *
            light.PDF_Li(lighting.lastLightCtx, wi, true);
        float mis = (lastSamplePDF > EPS && bounce > 0) ? PowerHeuristic(1, lastSamplePDF, 1, pdf) : 1.0f;
        AddRadianceByCategory(radiance, beta * Le * mis, bounce, firstBounceCategory, AOVCategory::Specular, false);
    }
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

bool EvaluateOpacityAlphaTest<S : ISampler>(uint instance, uint primitive, float2 bary, inout S rng)
{
    if (IsLightInstance(instance)) return false;
    GSInstance inst = LoadSceneInstance(instance);
    GSMaterial material = LoadMaterial(inst.materialIndex);

    float opacity = material.baseColorFactor.a;
    bool sampleBaseColor = material.baseColorTexture != ~0u;
    bool sampleTransmission = material.transmissionTexture != ~0u;
    float2 uv = float2(0.0f);
    if (sampleBaseColor || sampleTransmission)
        uv = GetHitUV(inst.resourceOffset, primitive, bary, inst);
    if (sampleBaseColor)
    {
        opacity *= gTextures2D[NonUniformResourceIndex(material.baseColorTexture)].SampleLevel(gTexSampler, uv, 0).a;
    }

    if (opacity == 1.0f) return false;    // opaque, don't ignore
    return rng.sample() < 1.0f - opacity; // true if we should ignore the hit
}

