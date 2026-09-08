import IBindless;
import ICommon;
import IBxDF;
import IBSDF;
import IFlags;
import IMath;
[[SpecializationConstant]] int flags;

uniform UBO globalParams;
StructuredBuffer<GSLight> lights;
Texture2D<float4> RT0;
Texture2D<float4> RT1;
Texture2D<float4> RT2;
Texture2D<float> depth;
Texture2D<float> ambientOcclusion;
RWTexture2D<float4> diffuseOutput;
RWTexture2D<float4> specularOutput;
RaytracingAccelerationStructure AS;

static const uint kMeshMask = 0x01;
static const uint kCurveMask = 0x04;
static const uint kTileWidth = 16;
static const uint kTileThreadCount = kTileWidth * kTileWidth;
static const uint kTileLightCapacity = 256;

struct TileBounds
{
    float3 minPosition;
    float3 maxPosition;
};

groupshared TileBounds tileBounds[kTileThreadCount];
groupshared uint tileLightCount;
groupshared uint tileLightOverflow;
groupshared uint tileLightIndices[kTileLightCapacity];

bool SphereIntersectsAABB(float3 center, float radius, TileBounds bounds)
{
    float3 closest = clamp(center, bounds.minPosition, bounds.maxPosition);
    float3 delta = center - closest;
    return dot(delta, delta) <= radius * radius;
}

bool shadow(float3 p, float3 l, float tMax = INF)
{
    RayDesc ray;
    ray.Origin = p;
    ray.Direction = l;
    ray.TMin = EPS_TMin;
    ray.TMax = tMax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(AS, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, kMeshMask | kCurveMask, ray);
    while (q.Proceed()){
        // Not alpha tested. A hit is a shadow.
        q.CommitNonOpaqueTriangleHit();
    }
    bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    return hit;
}

float EnvLightAzimuth()
{
    return lights[globalParams.lights.offset].params.x / 180.0f * Pi;
}

float3 EnvLightTint()
{
    GSLight gl = lights[globalParams.lights.offset];
    return gl.color * gl.power;
}

float ReflectionMipFromRoughness(float roughness, uint32_t numMips)
{
    const float kRoughestMip = 1.0f;
    const float kScale = 1.2f;
    float levelFrom1x1 = kRoughestMip - kScale * log2(max(roughness, 0.001f));
    return float(numMips - 1u) - levelFrom1x1;
}

[shader("compute")]
[numthreads(kTileWidth, kTileWidth, 1)]
void main(uint2 tid: SV_DispatchThreadID, uint groupIndex: SV_GroupIndex) {
    bool validPixel = tid.x < globalParams.fbWidth && tid.y < globalParams.fbHeight;
    float4 color0 = 0.0f;
    float4 color1 = 0.0f;
    float4 color2 = 0.0f;
    float ndcDepth = 0.0f;
    if (validPixel) {
        color0 = RT0.Load(uint3(tid,0));
        color1 = RT1.Load(uint3(tid,0));
        color2 = RT2.Load(uint3(tid,0));
        ndcDepth = depth.Load(uint3(tid,0));
    }

    bool hasSurface = validPixel && ndcDepth > EPS;
    float3 p = 0.0f;
    if (hasSurface) {
        float4 ndcPosition = float4(tid.xy / float2(globalParams.fbWidth, globalParams.fbHeight), ndcDepth, 1.0f);
        ndcPosition.y = 1 - ndcPosition.y;
        ndcPosition.xy = ndcPosition.xy * 2.0f - 1.0f;
        float4 wsPosition = mul(globalParams.inverseViewProj, ndcPosition);
        p = wsPosition.xyz / wsPosition.w;
    }

    const uint bypassLightingFlags = uint32_t(ViewFlagsBits.BaseColor) | uint32_t(ViewFlagsBits.Meshlet) |
                                     uint32_t(ViewFlagsBits.TextureLOD) | uint32_t(ViewFlagsBits.Normal) |
                                     uint32_t(ViewFlagsBits.Matcap) | uint32_t(ViewFlagsBits.Position);
    bool showLightTiles = (flags & uint32_t(ViewFlagsBits.LightTiles)) != 0;
    if (showLightTiles || (flags & bypassLightingFlags) == 0) {
        if (groupIndex == 0u) {
            tileLightCount = 0u;
            tileLightOverflow = 0u;
        }
        tileBounds[groupIndex].minPosition = hasSurface ? p : float3(INF);
        tileBounds[groupIndex].maxPosition = hasSurface ? p : float3(-INF);
        GroupMemoryBarrierWithGroupSync();

        for (uint stride = kTileThreadCount / 2u; stride > 0u; stride >>= 1u) {
            if (groupIndex < stride) {
                tileBounds[groupIndex].minPosition =
                    min(tileBounds[groupIndex].minPosition, tileBounds[groupIndex + stride].minPosition);
                tileBounds[groupIndex].maxPosition =
                    max(tileBounds[groupIndex].maxPosition, tileBounds[groupIndex + stride].maxPosition);
            }
            GroupMemoryBarrierWithGroupSync();
        }

        TileBounds bounds = tileBounds[0];
        bool tileHasSurface = bounds.minPosition.x <= bounds.maxPosition.x;
        for (uint lightIndex = groupIndex; lightIndex < globalParams.lights.count;
             lightIndex += kTileThreadCount) {
            GSLight light = lights[globalParams.lights.offset + lightIndex];
            uint lightType = GSLightType(light);
            if (lightType == kGSLightTypeEnvironment || !any(light.color * light.power > 0.0f))
                continue;

            bool intersects = tileHasSurface;
            if (intersects && lightType != kGSLightTypeDirectional && light.params.w > 0.0f)
                intersects = SphereIntersectsAABB(light.position, light.params.w, bounds);
            if (!intersects)
                continue;

            uint slot;
            InterlockedAdd(tileLightCount, 1u, slot);
            if (slot < kTileLightCapacity)
                tileLightIndices[slot] = lightIndex;
            else
                InterlockedOr(tileLightOverflow, 1u);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (!validPixel)
        return;

    if (showLightTiles) {
        float load = saturate(log2(float(tileLightCount) + 1.0f) / log2(float(kTileLightCapacity) + 1.0f));
        float3 tileColor = lerp(float3(0.0f, 0.08f, 0.35f), float3(0.0f, 1.0f, 0.15f),
                                saturate(load * 2.0f));
        tileColor = lerp(tileColor, float3(1.0f, 0.05f, 0.0f), saturate(load * 2.0f - 1.0f));
        if (tileLightOverflow != 0u)
            tileColor = float3(1.0f, 0.0f, 1.0f);
        if ((tid.x & (kTileWidth - 1u)) == 0u || (tid.y & (kTileWidth - 1u)) == 0u)
            tileColor *= 0.2f;
        diffuseOutput[tid] = float4(tileColor, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 baseColor = color0.xyz;
    if (ndcDepth <= EPS) {
        diffuseOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        float3 envLight = EnvLightTint();
        if (globalParams.hasEnvMap != 0u &&
            GSLightHasFlag(lights[globalParams.lights.offset], uint32_t(GSLightFlagsBits.EnvironmentMap))) {
            float2 uvNdc = (tid.xy + 0.5f) / float2(globalParams.fbWidth, globalParams.fbHeight);
            float4 ndcFar = float4(uvNdc.x * 2.0f - 1.0f, 1.0f - uvNdc.y * 2.0f, 1.0f, 1.0f);
            float4 wsFar = mul(globalParams.inverseViewProj, ndcFar);
            float3 dir = normalize(wsFar.xyz / wsFar.w - globalParams.camPosition.xyz);
            float azimuth = EnvLightAzimuth();
            float3 Le = gTextures2D[NonUniformResourceIndex(globalParams.envMapTextureIndex)]
                           .SampleLevel(gLutSampler, EnvMapDirToUV(dir, azimuth), 0).xyz;
            envLight = Le * EnvLightTint();
        }
        specularOutput[tid] = float4(envLight, 1.0f);
        return;
    }

    float3 eye = globalParams.camPosition.xyz;

    float3 normalWS = unpackUnitOctahedralSnorm(color1.xy * 2.0f - 1.0f);

    float metallic = color1.z;
    float roughness = color0.w;
    float3 emissive = color2.xyz;

    if ((flags & (uint32_t(ViewFlagsBits.BaseColor) | uint32_t(ViewFlagsBits.Meshlet) |
                  uint32_t(ViewFlagsBits.TextureLOD))) != 0) {
        // Meshlet and Texture LOD are passed down to GBuffer RT0.
        diffuseOutput[tid] = float4(baseColor, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    } else if ((flags & uint32_t(ViewFlagsBits.Normal)) != 0) {
        diffuseOutput[tid] = float4(normalWS * 0.5f + 0.5f, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    } else if ((flags & uint32_t(ViewFlagsBits.Matcap)) != 0) {
        // https://projects.blender.org/blender/blender/src/commit/e1d15b8920616df11437fd24189064275995b6f6/source/blender/draw/engines/workbench/shaders/workbench_matcap.bsl.hh#L13
        float3 normalVS = normalize(mul((float3x3)globalParams.view, normalWS));
        float2 uv = saturate(normalVS.xy * 0.496f + 0.5f);
        uv.y = 1.0 - uv.y; // Vulkan UV coords
        float3 matcap = float3(1.0f, 1.0f, 1.0f);
        if (globalParams.matcapTextureIndex != ~0u)
            matcap = gTextures2D[NonUniformResourceIndex(globalParams.matcapTextureIndex)].SampleLevel(gLutSampler, uv, 0.0f).xyz;
        diffuseOutput[tid] = float4(matcap * baseColor, 1.0f);
        specularOutput[tid] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    } else if ((flags & uint32_t(ViewFlagsBits.Position)) != 0) {
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

        BasicPBRBSDF bsdf = BasicPBRBSDF(baseColor, metallic, roughness);

        bool useFullLightList = tileLightOverflow != 0u;
        uint lightCount = useFullLightList ? globalParams.lights.count : min(tileLightCount, kTileLightCapacity);
        // TODO: Area lights. That's probably going to be ReSTIR DI if we'd want soft shadows from those...
        for (uint li = 0; li < lightCount; li++) {
            uint lightIndex = useFullLightList ? li : tileLightIndices[li];
            GSLight gl = lights[globalParams.lights.offset + lightIndex];
            uint32_t lightType = GSLightType(gl);
            float3 lightColor = gl.color * gl.power;
            if (!any(lightColor > 0.0f)) continue;
            if (lightType == kGSLightTypeEnvironment) // Environment: handled as IBL below.
                continue;

            float3 l;
            float atten = 1.0f;
            float lightDist = INF;
            if (lightType == kGSLightTypeDirectional) { // Directional
                l = -gl.direction;
            } else { // Point or Spot
                float3 toLight = gl.position - p;
                float dist2 = dot(toLight, toLight);
                if (dist2 < 1e-8f) continue;
                float dist = sqrt(dist2);
                l = toLight / dist;
                lightDist = dist;
                atten = 1.0f / dist2;
                if (gl.params.w > 0.0f) {
                    float normalizedDist2 = dist2 / (gl.params.w * gl.params.w);
                    if (normalizedDist2 >= 1.0f)
                        continue;
                    float rangeFalloff = 1.0f - normalizedDist2 * normalizedDist2;
                    atten *= rangeFalloff * rangeFalloff;
                }
                if (lightType == kGSLightTypeSpot) { // Spot
                    float cosTheta = dot(gl.direction, -l);
                    if (cosTheta < gl.params.z) continue;
                    if (cosTheta < gl.params.y) {
                        float delta = (cosTheta - gl.params.z) / (gl.params.y - gl.params.z);
                        atten *= (delta * delta) * (delta * delta);
                    }
                }
            }

            float3 wi = toLocal.rotate(l);
            if (!SameHemisphere(wo, wi))
                continue;
            float3 fd;
            float3 fs;
            bsdf.f(wo, wi, TransportMode::Radiance, fd, fs);
            float vis = 1.0f;
            if ((flags & uint32_t(ViewFlagsBits.EnableRasterRTShadows)) != 0 &&
                GSLightHasFlag(gl, uint32_t(GSLightFlagsBits.UseShadow))) {
                if (lightType < kGSLightTypeDisk /* Anything not area lights */) {
                    float3 shadowNormalFF = dot(l, normalWS) < 0.0f ? -normalWS : normalWS;
                    float3 shadowOrigin = OffsetRay(
                        p + shadowNormalFF * globalParams.rasterRTShadowBias, shadowNormalFF);
                    vis = shadow(shadowOrigin, l, lightDist) ? 0.0f : 1.0f;
                }
            }
            else
                vis = 1.0f;
            float3 lightContrib = AbsCosTheta(wi) * vis * lightColor * atten;
            Ld += fd * lightContrib;
            Ls += fs * lightContrib;
        }

        float3 envTint = EnvLightTint();
        bool envHasEnvMap = globalParams.hasEnvMap != 0u &&
            GSLightHasFlag(lights[globalParams.lights.offset], uint32_t(GSLightFlagsBits.EnvironmentMap));
        float3 diffuseAlbedo = baseColor * (1.0f - metallic);
        float ao = (flags & uint32_t(ViewFlagsBits.EnableRasterAmbientOcclusion)) != 0
                     ? ambientOcclusion.Load(uint3(tid, 0))
                     : 1.0f;
        Ld += ao * diffuseAlbedo * (envHasEnvMap ? SHIrradiance(globalParams.envSHCoeffs, normalWS) * envTint : envTint);

        float NoV = clamp(dot(normalWS, v), 0.0f, 1.0f);
        float3 F0 = lerp(float3(F0FromIor(1.5f)), baseColor, metallic);
        float2 lutE = SampleGGXLutE(globalParams.ggxLutEIndex, NoV, roughness);
        float3 F = F0 * lutE.x + (1.0f - F0) * lutE.y;
        if (envHasEnvMap) {
            float3 R = 2.0f * NoV * normalWS - v;
            float mip = ReflectionMipFromRoughness(roughness, globalParams.envMapPrefilteredMips);
            float azimuth = EnvLightAzimuth();
            float3 prefiltered = gTextures2D[NonUniformResourceIndex(globalParams.envMapTextureIndex)]
                                     .SampleLevel(gLutSampler, EnvMapDirToUV(R, azimuth), mip).xyz;
            Ls += prefiltered * envTint * F;
        } else
            Ls += envTint * F;

        Ld += emissive;

        diffuseOutput[tid] = float4(Ld, 1.0f);
        specularOutput[tid] = float4(Ls, 1.0f);
    }
}