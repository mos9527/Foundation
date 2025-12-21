// References
// - PBRT v4
// - RAY TRACING GEMS II - Chapter 14
// - https://github.com/boksajak/referencePT/blob/master/shaders/PathTracer.hlsl
// - https://github.com/nvpro-samples/vk_gltf_renderer/blob/master/shaders/gltf_pathtrace.slang
import IFlags;
import IMath;
import ICommon;
import IBRDF;
uniform UBO globalParams;
// -- Scene
RaytracingAccelerationStructure AS;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitives;
StructuredBuffer<GSMaterial> materials;
SamplerState textureSampler;
RWTexture2D<float4> accumulation;
[[vk_binding(0,1)]] Texture2D<float4> textures[];

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
    // vv Orthogonalize tangent and bitangent
    // res.tangent = normalize(res.tangent - dot(res.tangent, res.normal) * res.normal);
    // res.bitangent = cross(res.normal, res.tangent) * (payload.tangent.w > 0.0f ? 1 : -1);
    buildOrthonormalBasis(-res.normal, res.tangent, res.bitangent);
    return res;
}
float3 EvalSkyDome(float3 dir)
{
    return float3(1.0f); // Furnace
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
    for (uint bounce = 0; bounce < globalParams.ptMaxBounces; bounce++){
        HitPayload payload;
        TraceRay(AS, 0, 0xFF, 0, 0, 0, ray, payload);
        // Outgoing direction
        float3 view = -ray.Direction;
        if (!payload.IsHit()){
            // Miss - environment
            L += beta * EvalSkyDome(ray.Direction);
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
        float3x3 tbn = float3x3(hit.tangent, hit.bitangent, hit.normal);
        float3x3 toWorld     = tbn;
        float3x3 toLocal     = transpose(tbn);
        float3 n = float3(0,0,1);
        float3 wo = normalize(mul(toLocal, view));
        float3 wi = float3(0);
        // Emissive
        L += beta * hit.emissive;
        BSDFSample sample = glTFSample(rng, wo, hit.baseColor, hit.metallic, hit.roughness, 1.0f);
        if (!sample.IsValid()){
            // L = float3(1,0,0);
            break;
        }
        wi = sample.wi;
        beta *= sample.f * AbsCosTheta(wi) / sample.pdf;
        // Select next ray to sample from current position
        ray.Direction = normalize(mul(toWorld, wi));
        ray.Origin = OffsetRay(hit.position, hit.geometryNormal);
        if (dot(LUMINANCE_RGB,beta) < EPS)
            break;
    }
    // Transform world space point
    // Accumulation
    float3 output = L;
    float frameCount = float(globalParams.ptAccumualatedFrames + 1);
    float3 prevAvg = (globalParams.ptAccumualatedFrames == 0) ? float3(0) : accumulation[pix].xyz;
    // Welford mean update
    float3 newAvg = prevAvg + (output - prevAvg) / frameCount;
    accumulation[pix] = float4(newAvg, 1.0);
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
