#include "ICommon.slang"
#include "IBSDF.slang"
import IBindless2D;
import IFlags;
import IMath;
[[SpecializationConstant]] int flags;

uniform UBO globalParams;
StructuredBuffer<GSLight> lights;
Texture2D<float4> RT0;
Texture2D<float4> RT1;
Texture2D<float4> RT2;
Texture2D<float> depth;
RWTexture2D<float4> diffuseOutput;
RWTexture2D<float4> specularOutput;
RaytracingAccelerationStructure AS;

bool shadow(float3 p, float3 l, float tMax = INF)
{
    RayDesc ray;
    ray.Origin = p;
    ray.Direction = l;
    ray.TMin = EPS_TMin;
    ray.TMax = tMax;
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
        diffuseOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float4 ndcPosition = float4(tid.xy / float2(globalParams.fbWidth, globalParams.fbHeight), ndcDepth, 1.0f);
    ndcPosition.y = 1 - ndcPosition.y;
    ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
    float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);

    float3 p = wsPosition.xyz / wsPosition.w;
    float3 eye = globalParams.camPosition.xyz;

    float3 normalWS, t, b;
    unpackTBN888(color1.xyz, normalWS, t);
    b = cross(normalWS, t) * (BSign == 1 ? 1.0f : -1.0f);

    float metallic = color1.w;
    float roughness = color2.w;
    float3 emissive = color2.xyz;

    if ((flags & (kViewBaseColor | kViewMeshlet | kViewTextureLOD)) != 0) {
        // Meshlet and Texture LOD are passed down to GBuffer RT0.
        diffuseOutput[tid] = float4(baseColor, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    } else if ((flags & kViewMaterialID) != 0) {
        diffuseOutput[tid] = float4(unpackUnorm4x8ToFloat(hash(materialID)).xyz, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    } else if ((flags & kViewNormal) != 0) {
        diffuseOutput[tid] = float4(normalWS * 0.5f + 0.5f, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    } else if ((flags & kViewMatcap) != 0) {
        // https://projects.blender.org/blender/blender/src/commit/e1d15b8920616df11437fd24189064275995b6f6/source/blender/draw/engines/workbench/shaders/workbench_matcap.bsl.hh#L13
        float3 normalVS = normalize(mul((float3x3)globalParams.view, normalWS));
        float2 uv = saturate(normalVS.xy * 0.496f + 0.5f);
        uv.y = 1.0 - uv.y; // Vulkan UV coords
        float3 matcap = float3(1.0f, 1.0f, 1.0f);
        if (globalParams.matcapTextureIndex != ~0u)
            matcap = textures[NonUniformResourceIndex(globalParams.matcapTextureIndex)].SampleLevel(lutSampler, uv, 0.0f).xyz;
        diffuseOutput[tid] = float4(matcap * baseColor, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    } else if ((flags & kViewPosition) != 0) {
        diffuseOutput[tid] = float4(p.xyz, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    } else {
        float3 v = normalize(eye - p);

        float3 Ld = 0;
        float3 Ls = 0;

        // Direct lighting from scene lights
        quat toLocal = quatRotateToZ(normalWS);
        float3 wo = toLocal.rotate(v);

        PrincipledBSDF bsdf = PrincipledBSDF(baseColor, metallic, roughness, 1.5f, 1.0f, 0.0f, globalParams.energyCompensation != 0u, wo,
                                             1.0f, 0.0f, float3(1.0f), globalParams.ggxLutEIndex, globalParams.ggxLutEavgIndex, globalParams.ggxLutEIORIndex, globalParams.ggxLutEIORavgIndex, globalParams.ggxLutEIORInvIndex, globalParams.ggxLutEIORInvavgIndex,
                                             0.0f, 0.0f, float3(1.0f), globalParams.sheenLtcIndex);

        // TODO: Area lights. That's probably going to be ReSTIR DI if we'd want soft shadows from those...
        for (uint li = 0; li < globalParams.numSceneLights; li++) {
            GSLight gl = lights[globalParams.firstLight + li];
            float3 lightColor = gl.color * gl.power;
            if (!any(lightColor > 0.0f)) continue;
            if (gl.type == 5) { // Environment: raster uses the synthetic env light as simple ambient.
                Ld += baseColor * lightColor;
                continue;
            }

            float3 l;
            float atten = 1.0f;
            float lightDist = INF;
            if (gl.type == 0) { // Directional
                l = -gl.direction;
            } else { // Point or Spot
                float3 toLight = gl.position - p;
                float dist2 = dot(toLight, toLight);
                if (dist2 < 1e-8f) continue;
                float dist = sqrt(dist2);
                l = toLight / dist;
                lightDist = dist;
                atten = 1.0f / dist2;
                if (gl.type == 1 || gl.type == 2)
                    if (gl.range > 0.0f && dist > gl.range) continue;
                if (gl.type == 2) { // Spot
                    float cosTheta = dot(gl.direction, -l);
                    if (cosTheta < gl.spotOuterCosAngle) continue;
                    if (cosTheta < gl.spotInnerCosAngle) {
                        float delta = (cosTheta - gl.spotOuterCosAngle) / (gl.spotInnerCosAngle - gl.spotOuterCosAngle);
                        atten *= (delta * delta) * (delta * delta);
                    }
                }
            }

            float3 wi = toLocal.rotate(l);
            float3 fd;
            float3 fs;
            bsdf.f(wo, wi, TransportMode::Radiance, fd, fs);
            float vis = 1.0f;
            if ((flags & kEnableRasterRTShadows) != 0) {
                if (gl.type <= 2 /* Anything not area lights */) {
                    float3 shadowNormalFF = dot(l, normalWS) < 0.0f ? -normalWS : normalWS;
                    float3 shadowOrigin = OffsetRay(p, shadowNormalFF);
                    vis = shadow(shadowOrigin, l, lightDist) ? 0.0f : 1.0f;
                }
            }
            else
                vis = 1.0f;
            float3 lightContrib = AbsCosTheta(wi) * vis * lightColor * atten;
            Ld += fd * lightContrib;
            Ls += fs * lightContrib;
        }

        Ld += emissive;

        diffuseOutput[tid] = float4(Ld, 1.0f);
        specularOutput[tid] = float4(Ls, 1.0f);
    }
}