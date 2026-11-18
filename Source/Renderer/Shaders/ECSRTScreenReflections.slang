import ICommon;
import IFlags;
import IMath;
import IBSDF;
import IBxDF;
import IScreenSpace;
import ISceneRay;

uniform UBO globalParams;
RaytracingAccelerationStructure scene;
ByteAddressBuffer instances;
ByteAddressBuffer materials;
ByteAddressBuffer staticPrimitives;
ByteAddressBuffer dynamicPrimitives;
Texture2D<float4> RT0;
Texture2D<float4> RT1;
Texture2D<float> depth;
Texture2D<uint> instanceID;
Texture2D<float4> sourceDiffuse;
Texture2D<float4> sourceSpecular;
Texture2D<float4> environmentSpecular;
Texture2D<float4> sceneRadiance;
SamplerState radianceSampler;
SamplerState materialSampler;
RWTexture2D<float4> outputSpecular;

struct ReflectionPushConstants
{
    float maxDistance;
    float originBias;
    float relativeDepthTolerance;
    float coneScale;
    float maxRoughness;
    float screenFade;
    float strength;
    uint mipLevels;
};
[[vk::push_constant]] ReflectionPushConstants pc;

float3 SampleScreenRadiance(float2 uv, float footprintPixels)
{
    float fullResolutionLOD = max(log2(max(footprintPixels, 1.0f)), 0.0f);
    uint baseWidth, baseHeight;
    sceneRadiance.GetDimensions(baseWidth, baseHeight);
    // Base cells correspond to aligned 2x2 full-res quads, including a partial final cell.
    float2 pyramidUV = uv * float2(globalParams.fbWidth, globalParams.fbHeight) /
        (2.0f * float2(baseWidth, baseHeight));
    // GGX-prefiltered level m has lobe diameter 2^(m+1) full-res pixels.
    // This is a spatial footprint lookup, not the environment map's roughness-to-mip mapping.
    float lod = clamp(fullResolutionLOD - 1.0f, 0.0f, float(pc.mipLevels - 1u));
    float3 filtered = sceneRadiance.SampleLevel(radianceSampler, pyramidUV, lod).rgb;
    if (fullResolutionLOD < 1.0f)
    {
        float3 sharp = sourceDiffuse.SampleLevel(radianceSampler, uv, 0.0f).rgb +
                       sourceSpecular.SampleLevel(radianceSampler, uv, 0.0f).rgb;
        return lerp(sharp, filtered, fullResolutionLOD);
    }
    return filtered;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    uint2 extent = uint2(uint(globalParams.fbWidth), uint(globalParams.fbHeight));
    if (any(pixel >= extent))
        return;
    float3 originalSpecular = sourceSpecular.Load(int3(pixel, 0)).rgb;
    // Seed the exact existing environment/direct result before any rejection.
    outputSpecular[pixel] = float4(originalSpecular, 1.0f);
    float deviceDepth = depth.Load(int3(pixel, 0));
    float4 baseRoughness = RT0.Load(int3(pixel, 0));
    float roughness = baseRoughness.a;
    if (deviceDepth <= EPS || roughness >= pc.maxRoughness || pc.strength <= 0.0f ||
        (globalParams.dbgMaterialFlags & uint32_t(MaterialFlagsBits.DbgDiffuseOnly)) != 0u)
        return;

    float4 normalMetallic = RT1.Load(int3(pixel, 0));
    float3 normal = unpackUnitOctahedralSnorm(normalMetallic.xy * 2.0f - 1.0f);
    float3 position = ReconstructScreenPosition(pixel, deviceDepth, float2(extent), globalParams.inverseViewProj);
    float3 viewDirection = normalize(globalParams.camPosition.xyz - position);
    float NoV = dot(normal, viewDirection);
    if (NoV <= 0.0f)
        return;
    RayDesc ray;
    ray.Direction = normalize(reflect(-viewDirection, normal));
    ray.Origin = OffsetRay(position + normal * pc.originBias, normal);
    ray.TMin = EPS_TMin;
    ray.TMax = pc.maxDistance;
    SceneRayHit hit = TraceSceneClosest(scene, ray, globalParams, instances, materials,
                                       staticPrimitives, dynamicPrimitives, materialSampler, pixel);
    // Misses AND unavailable screen radiance fall back to the prefiltered environment already in lighting.
    if (!hit.valid)
        return;
    float2 hitUV;
    float hitClipW;
    if (!ProjectScreenPosition(hit.position, globalParams.view, globalParams.proj, hitUV, hitClipW))
        return;
    uint2 hitPixel = min(uint2(hitUV * float2(extent)), extent - 1u);
    if (instanceID.Load(int3(hitPixel, 0)) != hit.instance)
        return;
    float hitDeviceDepth = depth.Load(int3(hitPixel, 0));
    if (hitDeviceDepth <= EPS)
        return;

    float3 visiblePosition = ReconstructScreenPosition(hitPixel, hitDeviceDepth, float2(extent),
                                                       globalParams.inverseViewProj);
    float3 hitNormal = unpackUnitOctahedralSnorm(RT1.Load(int3(hitPixel, 0)).xy * 2.0f - 1.0f);
    float hitViewDepth = abs(mul(globalParams.view, float4(hit.position, 1.0f)).z);
    float visibleViewDepth = abs(mul(globalParams.view, float4(visiblePosition, 1.0f)).z);
    float focalPixels = max(abs(globalParams.proj[0][0]) * float(extent.x),
                           abs(globalParams.proj[1][1]) * float(extent.y)) * 0.5f;
    // Clip w also handles orthographic projections (w=1), unlike a hardcoded distance/FOV formula.
    float hitPixelWorldSize = hitClipW / max(focalPixels, 1e-4f);
    float3 cameraToHit = normalize(hit.position - globalParams.camPosition.xyz);
    float tolerance = max(pc.relativeDepthTolerance * hitViewDepth, 1e-4f) +
                      hitPixelWorldSize / max(abs(dot(hitNormal, cameraToHit)), 0.1f);
    float depthError = abs(hitViewDepth - visibleViewDepth);
    if (depthError >= tolerance)
        return;

    float4 sourceClip = mul(globalParams.proj, mul(globalParams.view, float4(position, 1.0f)));
    float initialRadius = sourceClip.w / max(focalPixels, 1e-4f) * 0.5f;
    // GGX alpha=roughness^2 describes half-vector slopes; reflection doubles the
    // small-angle lobe width. Match that width to the image-plane GGX prefilter.
    float coneRadius = initialRadius + 2.0f * hit.distance * roughness * roughness * pc.coneScale;
    float footprint = 2.0f * coneRadius / max(hitPixelWorldSize, 1e-6f);
    // Approximate the major axis of the cone's footprint on a grazing hit surface.
    footprint /= max(abs(dot(hitNormal, -ray.Direction)), 0.2f);
    float2 edgeDistance = min(hitUV, 1.0f - hitUV);
    float confidence = pc.screenFade > 0.0f
        ? saturate(min(edgeDistance.x, edgeDistance.y) / pc.screenFade) : 1.0f;
    confidence *= 1.0f - smoothstep(0.5f, 1.0f, depthError / tolerance);
    confidence *= 1.0f - smoothstep(max(pc.maxRoughness - 0.1f, 0.0f), pc.maxRoughness, roughness);
    confidence *= pc.strength;

    float3 F0 = lerp(float3(F0FromIor(1.5f)), baseRoughness.rgb, normalMetallic.z);
    float2 lut = SampleGGXLutE(globalParams.ggxLutEIndex, saturate(NoV), roughness);
    float3 weight = F0 * lut.x + (1.0f - F0) * lut.y;
    float3 screenReflection = SampleScreenRadiance(hitUV, footprint) * weight;
    float3 oldEnvironment = environmentSpecular.Load(int3(pixel, 0)).rgb;
    // Replace only indirect environment specular. Direct highlights remain untouched.
    float3 directSpecular = max(originalSpecular - oldEnvironment, 0.0f);
    outputSpecular[pixel] = float4(directSpecular + lerp(oldEnvironment, screenReflection, confidence), 1.0f);
}
