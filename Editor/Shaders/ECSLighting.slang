#include "ICommon.slang"
#include "IBSDF.slang"
import IFlags;
import IMath;
[[SpecializationConstant]] int flags;

uniform UBO globalParams;
Texture2D<float4> RT0;
Texture2D<float4> RT1;
Texture2D<float4> RT2;
Texture2D<float> depth;
RWTexture2D<float4> output;
RaytracingAccelerationStructure AS;

bool shadow(float3 p, float3 l)
{
    RayDesc ray;
    ray.Origin = p;
    ray.Direction = l;
    ray.TMin = 1e-2;
    ray.TMax = 1e2;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(AS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);
    while (q.Proceed()){
        // Not alpha tested. A hit is a shadow.
        q.CommitNonOpaqueTriangleHit();
    }
    bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    return hit;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint2 tid: SV_DispatchThreadID) {
    if (tid.x >= globalParams.fbWidth || tid.y >= globalParams.fbHeight) return;
    float4 color0 = RT0.Load(uint3(tid,0));
    float4 color1 = RT1.Load(uint3(tid,0));
    float4 color2 = RT2.Load(uint3(tid,0));


    float3 baseColor = color0.xyz;
    uint32_t materialID = (uint)(color0.w * 255.0f), BSign;
    BSign = bitfieldExtract(materialID, 7, 1);
    materialID = bitfieldExtract(materialID, 0, 7);

    float ndcDepth = depth.Load(uint3(tid,0));
    if (ndcDepth <= EPS) {
        // Infinite depth
        output[tid] = float4(globalParams.ambientColor, 1.0f);
        return;
    }

    float4 ndcPosition = float4(tid.xy / float2(globalParams.fbWidth, globalParams.fbHeight), ndcDepth, 1.0f);
    ndcPosition.y = 1 - ndcPosition.y;
    ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
    float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);

    float3 p = wsPosition.xyz / wsPosition.w;
    float3 eye = globalParams.camPosition;

    float3 n, t, b;
    unpackTBN888(color1.xyz, n, t);
    b = cross(n, t) * (BSign == 1 ? 1.0f : -1.0f);

    float metallic = color1.w;
    float roughness = color2.w;
    float3 emissive = color2.xyz;

    if (flags & (kViewBaseColor | kViewMeshlet)) {
        // Meshlet passed down to GBuffer RT0
        output[tid] = float4(baseColor, 1.0f);
        return;
    } else if (flags & kViewMaterialID) {
        output[tid] = float4(unpackUnorm4x8ToFloat(hash(materialID)).xyz, 1.0f);
        return;
    } else if (flags & kViewNormal) {
        output[tid] = float4(n * 0.5f + 0.5f, 1.0f);
        return;
    } else if (flags & kViewPosition) {
        output[tid] = float4(p.xyz, 1.0f);
        return;
    } else {
        float3 v = normalize(eye - p);
        float3 l = -globalParams.sunDirection;

        float3 L = 0;

        // Direct
        PrincipledBSDF bsdf = PrincipledBSDF(baseColor, metallic, roughness);
        quat toLocal     = quatRotateToZ(n);
        quat toWorld     = toLocal.inverted();
        float3 n = float3(0,0,1);
        float3 wi = toLocal.rotate(l);
        float3 wo = toLocal.rotate(v);
        float3 beta = bsdf.f(wo, wi) * AbsCosTheta(wi);
        float pdf = shadow(p, l) ? 0.0f : 1.0f;
        L += beta * pdf /* dirac - this formulation on itself is not accurate */ * globalParams.sunIntensity;

        // Indirect
        // This needs to be a proper IBL someday.
        // We've long prepared the Eavg parts, however. I'll implement this once I finish pre-filtering as well.

        // Final color
        output[tid] = float4(L + emissive, 1.0f);
    }
}