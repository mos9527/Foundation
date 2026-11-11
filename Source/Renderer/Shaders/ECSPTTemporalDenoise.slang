import IMath;
import IPTCamera;

// Mirrored in PathtracerDenoiser.cpp. Camera state belongs to the last traced image,
// not necessarily the immediately preceding display frame (rendering can be paused).
struct PTDenoiseFrameConstants
{
    float4x4 view;
    float4x4 inverseView;
    float4 cameraPosition;
    float2 cameraTanHalfFov;
    uint frameNumber;
    uint historyEpoch;
    uint supported;
    uint resetHistory;
    uint sampleNumber;
    uint padding;
};
ConstantBuffer<PTDenoiseFrameConstants> currentFrame;
ConstantBuffer<PTDenoiseFrameConstants> previousFrame;

struct PTTemporalDenoisePushConstants
{
    uint historyValid;
    uint maxDiffuseFrames;
    uint maxSpecularFrames;
    float relativeDepthThreshold;
    float normalThreshold;
    float albedoThreshold;
    float clampSigma;
    uint padding;
};
[[vk::push_constant]] PTTemporalDenoisePushConstants pc;

Texture2D<float4> currentDiffuse;
Texture2D<float4> currentSpecular;
Texture2D<float4> currentNormalRoughness;
Texture2D<float4> currentAlbedoMetallic;
Texture2D<float> currentLinearDepth;
Texture2D<uint> currentInstanceID;
Texture2D<float4> historyDiffuse;
Texture2D<float4> historySpecular;
Texture2D<float4> historyNormalRoughness;
Texture2D<float4> historyAlbedoMetallic;
Texture2D<float> historyLinearDepth;
Texture2D<uint> historyInstanceID;
RWTexture2D<float4> outputDiffuse;
RWTexture2D<float4> outputSpecular;
RWTexture2D<float4> outputNormalRoughness;
RWTexture2D<float4> outputAlbedoMetallic;
RWTexture2D<float> outputLinearDepth;
RWTexture2D<uint> outputInstanceID;

bool TemporalFinite3(float3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

bool TemporalFinite4(float4 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

bool ValidSurfaceGuide(float depth, float4 normalRoughness, float4 albedoMetallic)
{
    return depth > 0.0f && !isnan(depth) && !isinf(depth) &&
        TemporalFinite4(normalRoughness) && dot(normalRoughness.xyz, normalRoughness.xyz) > 0.25f &&
        TemporalFinite4(albedoMetallic);
}

bool CompatibleMaterial(float4 centerAlbedo, float4 sampleAlbedo, float centerRoughness, float sampleRoughness)
{
    return abs(centerAlbedo.a - sampleAlbedo.a) <= 0.1f &&
        length(centerAlbedo.rgb - sampleAlbedo.rgb) <= pc.albedoThreshold &&
        ((centerRoughness < 0.0f) == (sampleRoughness < 0.0f)) &&
        abs(centerRoughness - sampleRoughness) <= 0.2f;
}

float TemporalDepthTolerance(float depth, float3 normal, float3 rayDirection, float2 tanHalfFov, int2 extent)
{
    // Both depth buffers contain radial distance from their own camera. Allow for
    // neighboring subpixel samples on sloped surfaces, but never compare current depth
    // directly with previous depth after camera translation.
    float footprint = 2.0f * depth * max(tanHalfFov.x / float(extent.x), tanHalfFov.y / float(extent.y));
    return max(pc.relativeDepthThreshold * depth, 1e-4f) +
        footprint / max(abs(dot(normal, rayDirection)), 0.1f);
}

struct HistoryBounds
{
    float3 diffuseMin;
    float3 diffuseMax;
    float3 specularMin;
    float3 specularMax;
};

HistoryBounds GetHistoryBounds(int2 pixel, int2 extent, uint instance, float depth, float depthTolerance,
                               float4 normalRoughness, float4 albedoMetallic, float3 diffuse, float3 specular)
{
    float3 normal = normalize(normalRoughness.xyz);
    float3 diffuseSum = diffuse;
    float3 specularSum = specular;
    float3 diffuseSum2 = diffuse * diffuse;
    float3 specularSum2 = specular * specular;
    float count = 1.0f;
    HistoryBounds bounds;
    bounds.diffuseMin = bounds.diffuseMax = diffuse;
    bounds.specularMin = bounds.specularMax = specular;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            int2 samplePixel = pixel + int2(x, y);
            if (any(samplePixel < 0) || any(samplePixel >= extent))
                continue;
            if (currentInstanceID.Load(int3(samplePixel, 0)) != instance)
                continue;
            float sampleDepth = currentLinearDepth.Load(int3(samplePixel, 0));
            float4 sampleNormal = currentNormalRoughness.Load(int3(samplePixel, 0));
            float4 sampleAlbedo = currentAlbedoMetallic.Load(int3(samplePixel, 0));
            if (!ValidSurfaceGuide(sampleDepth, sampleNormal, sampleAlbedo))
                continue;
            if (abs(depth - sampleDepth) > depthTolerance ||
                dot(normal, normalize(sampleNormal.xyz)) < pc.normalThreshold ||
                !CompatibleMaterial(albedoMetallic, sampleAlbedo, normalRoughness.w, sampleNormal.w))
                continue;
            float3 d = currentDiffuse.Load(int3(samplePixel, 0)).rgb;
            float3 s = currentSpecular.Load(int3(samplePixel, 0)).rgb;
            if (!TemporalFinite3(d) || !TemporalFinite3(s))
                continue;
            diffuseSum += d;
            specularSum += s;
            diffuseSum2 += d * d;
            specularSum2 += s * s;
            bounds.diffuseMin = min(bounds.diffuseMin, d);
            bounds.diffuseMax = max(bounds.diffuseMax, d);
            bounds.specularMin = min(bounds.specularMin, s);
            bounds.specularMax = max(bounds.specularMax, s);
            count += 1.0f;
        }
    }
    float3 diffuseMean = diffuseSum / count;
    float3 specularMean = specularSum / count;
    float3 diffuseSigma = sqrt(max(diffuseSum2 / count - diffuseMean * diffuseMean, float3(0.0f)));
    float3 specularSigma = sqrt(max(specularSum2 / count - specularMean * specularMean, float3(0.0f)));
    // A deliberately loose variance envelope: strict single-frame min/max clipping is
    // too aggressive for sparse 1-spp lighting, especially on newly visible surfaces.
    bounds.diffuseMin = max(float3(0.0f), min(bounds.diffuseMin, diffuseMean - pc.clampSigma * diffuseSigma));
    bounds.diffuseMax = max(bounds.diffuseMax, diffuseMean + pc.clampSigma * diffuseSigma);
    bounds.specularMin = max(float3(0.0f), min(bounds.specularMin, specularMean - pc.clampSigma * specularSigma));
    bounds.specularMax = max(bounds.specularMax, specularMean + pc.clampSigma * specularSigma);
    return bounds;
}

float4 AccumulateHistory(float3 current, float4 historySum, float coverage, float maxFrames,
                         float3 neighborhoodMin, float3 neighborhoodMax)
{
    if (coverage <= 1e-5f)
        return float4(current, 1.0f);
    float4 history = historySum / coverage;
    float previousSamples = max(history.a, 1.0f) * saturate(coverage);
    float samples = min(previousSamples + 1.0f, maxFrames);
    float historyWeight = 1.0f - 1.0f / samples;
    float3 clipped = clamp(history.rgb, neighborhoodMin, neighborhoodMax);
    return float4(lerp(current, clipped, historyWeight), samples);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint width, height;
    currentDiffuse.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height)
        return;
    int2 extent = int2(width, height);
    int2 pixel = int2(tid);
    int3 coord = int3(pixel, 0);

    if (pc.historyValid != 0u)
    {
        if (currentFrame.sampleNumber == previousFrame.sampleNumber)
        {
            // Temporal handles rotate even while paused. Copy both radiance and guides
            // forward, preserving counts instead of accumulating the same sample again.
            outputDiffuse[tid] = historyDiffuse.Load(coord);
            outputSpecular[tid] = historySpecular.Load(coord);
            outputNormalRoughness[tid] = historyNormalRoughness.Load(coord);
            outputAlbedoMetallic[tid] = historyAlbedoMetallic.Load(coord);
            outputLinearDepth[tid] = historyLinearDepth.Load(coord);
            outputInstanceID[tid] = historyInstanceID.Load(coord);
            return;
        }
    }

    float4 diffuse = currentDiffuse.Load(coord);
    float4 specular = currentSpecular.Load(coord);
    float4 normalRoughness = currentNormalRoughness.Load(coord);
    float4 albedoMetallic = currentAlbedoMetallic.Load(coord);
    float depth = currentLinearDepth.Load(coord);
    uint instance = currentInstanceID.Load(coord);
    outputDiffuse[tid] = diffuse;
    outputSpecular[tid] = specular;
    outputNormalRoughness[tid] = normalRoughness;
    outputAlbedoMetallic[tid] = albedoMetallic;
    outputLinearDepth[tid] = depth;
    outputInstanceID[tid] = instance;

    // Seed every output before rejecting history. Do not read any previous resource
    // on the first frame, after a graph rebuild, or when temporal resources are invalid.
    if (pc.historyValid == 0u || currentFrame.supported == 0u || currentFrame.resetHistory != 0u)
        return;
    if (previousFrame.supported == 0u || currentFrame.historyEpoch != previousFrame.historyEpoch)
        return;
    if (instance == ~0u || !ValidSurfaceGuide(depth, normalRoughness, albedoMetallic) ||
        !TemporalFinite4(diffuse) || !TemporalFinite4(specular))
        return;

    // This exactly replays the tracer's first camera sample. Linear guide depth is
    // ray distance, so unprojecting the pixel center as view/device Z would be wrong.
    PCG rng = PCG(tid, currentFrame.frameNumber);
    float2 pixelSample = PTSampleCameraPixel(tid, rng);
    float3 rayDirection = PTPinholeRayDirection(pixelSample, float2(extent),
                                               currentFrame.cameraTanHalfFov, currentFrame.inverseView);
    float3 worldPosition = currentFrame.cameraPosition.xyz + rayDirection * depth;
    float3 previousProjection = PTProjectPinhole(worldPosition, previousFrame.view, previousFrame.cameraTanHalfFov);
    if (!TemporalFinite3(previousProjection) || previousProjection.z <= 0.0f ||
        any(previousProjection.xy < 0.0f) || any(previousProjection.xy >= 1.0f))
        return;
    float3 previousRay = worldPosition - previousFrame.cameraPosition.xyz;
    float expectedDepth = length(previousRay);
    if (!(expectedDepth > 0.0f) || isnan(expectedDepth) || isinf(expectedDepth))
        return;
    float3 normal = normalize(normalRoughness.xyz);
    float depthTolerance = TemporalDepthTolerance(expectedDepth, normal, previousRay / expectedDepth,
                                                   previousFrame.cameraTanHalfFov, extent);
    float2 previousPixel = previousProjection.xy * float2(extent) - 0.5f;
    int2 basePixel = int2(floor(previousPixel));
    float2 fraction = frac(previousPixel);
    float4 diffuseHistorySum = float4(0.0f);
    float4 specularHistorySum = float4(0.0f);
    float diffuseCoverage = 0.0f;
    float specularCoverage = 0.0f;
    bool accumulateSpecular = normalRoughness.w >= 0.3f;
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            int2 samplePixel = basePixel + int2(x, y);
            if (any(samplePixel < 0) || any(samplePixel >= extent))
                continue;
            int3 sampleCoord = int3(samplePixel, 0);
            if (historyInstanceID.Load(sampleCoord) != instance)
                continue;
            float sampleDepth = historyLinearDepth.Load(sampleCoord);
            float4 sampleNormal = historyNormalRoughness.Load(sampleCoord);
            float4 sampleAlbedo = historyAlbedoMetallic.Load(sampleCoord);
            if (!ValidSurfaceGuide(sampleDepth, sampleNormal, sampleAlbedo))
                continue;
            float normalAgreement = dot(normal, normalize(sampleNormal.xyz));
            if (abs(expectedDepth - sampleDepth) > depthTolerance || normalAgreement < pc.normalThreshold ||
                !CompatibleMaterial(albedoMetallic, sampleAlbedo, normalRoughness.w, sampleNormal.w))
                continue;
            // Validate each bilinear tap before mixing; hardware bilinear filtering would
            // already have blended incompatible foreground/background history together.
            float weight = (x == 0 ? 1.0f - fraction.x : fraction.x) *
                           (y == 0 ? 1.0f - fraction.y : fraction.y);
            float4 d = historyDiffuse.Load(sampleCoord);
            if (TemporalFinite4(d) && d.a >= 1.0f)
            {
                diffuseHistorySum += d * weight;
                diffuseCoverage += weight;
            }
            if (accumulateSpecular && sampleNormal.w >= 0.3f &&
                abs(normalRoughness.w - sampleNormal.w) <= 0.1f && normalAgreement >= max(pc.normalThreshold, 0.96f))
            {
                float4 s = historySpecular.Load(sampleCoord);
                if (TemporalFinite4(s) && s.a >= 1.0f)
                {
                    specularHistorySum += s * weight;
                    specularCoverage += weight;
                }
            }
        }
    }
    if (diffuseCoverage <= 1e-5f && specularCoverage <= 1e-5f)
        return;
    float currentDepthTolerance = TemporalDepthTolerance(depth, normal, rayDirection, currentFrame.cameraTanHalfFov, extent);
    HistoryBounds bounds = GetHistoryBounds(pixel, extent, instance, depth, currentDepthTolerance,
                                            normalRoughness, albedoMetallic, diffuse.rgb, specular.rgb);
    outputDiffuse[tid] = AccumulateHistory(diffuse.rgb, diffuseHistorySum, diffuseCoverage, float(pc.maxDiffuseFrames),
                                          bounds.diffuseMin, bounds.diffuseMax);
    if (accumulateSpecular)
    {
        // Geometry-only reprojection is an approximation for view-dependent reflections.
        // Keep rough-specular history short, and reduce it further during camera motion.
        float motionPixels = length(previousProjection.xy * float2(extent) - pixelSample);
        float roughnessConfidence = saturate((normalRoughness.w - 0.3f) / 0.7f);
        float maxFrames = 1.0f + (float(pc.maxSpecularFrames) - 1.0f) * roughnessConfidence /
                                  (1.0f + motionPixels * 0.1f);
        outputSpecular[tid] = AccumulateHistory(specular.rgb, specularHistorySum, specularCoverage, maxFrames,
                                               bounds.specularMin, bounds.specularMax);
    }
}
