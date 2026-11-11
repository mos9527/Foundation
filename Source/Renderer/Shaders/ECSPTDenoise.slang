// Spatial-only, primary-hit-guided a-trous denoiser. All color inputs are linear HDR.
Texture2D<float4> inputDiffuse;
Texture2D<float4> inputSpecular;
Texture2D<float4> normalRoughness;
Texture2D<float4> albedoMetallic;
Texture2D<float> linearDepth;
Texture2D<uint> instanceID;
RWTexture2D<float4> outputDiffuse;
RWTexture2D<float4> outputSpecular;

struct PTDenoisePushConstants
{
    uint stepWidth;
    uint enabled;
    float normalPower;
    float relativeDepthSigma;
    float albedoSigma;
    float luminanceSigma;
};
[[vk::push_constant]] PTDenoisePushConstants pc;

static const float kKernel[5] = {1.0f, 4.0f, 6.0f, 4.0f, 1.0f};

bool IsFiniteRadiance(float3 value)
{
    return !any(isnan(value)) && !any(isinf(value));
}

float Luminance(float3 color)
{
    return max(dot(color, float3(0.2126f, 0.7152f, 0.0722f)), 0.0f);
}

float LuminanceWeight(float center, float sample)
{
    // Relative contrast avoids locking a noisy bright center sample in place.
    // Temporal moments/variance can replace this scale in a later temporal stage.
    float scale = pc.luminanceSigma * max(center + sample, 1e-4f);
    return exp(-abs(center - sample) / scale);
}

float EstimateDepthSlope(int2 pixel, int2 axis, int2 extent, uint centerID, float centerDepth)
{
    float slope = 0.0f;
    bool found = false;
    for (int sign = -1; sign <= 1; sign += 2)
    {
        int2 neighbor = pixel + axis * sign;
        if (any(neighbor < 0) || any(neighbor >= extent))
            continue;
        if (instanceID.Load(int3(neighbor, 0)) != centerID)
            continue;
        float depth = linearDepth.Load(int3(neighbor, 0));
        if (!(depth > 0.0f) || isnan(depth) || isinf(depth))
            continue;
        // The smaller one-sided difference avoids treating a silhouette as a steep plane.
        float delta = abs(depth - centerDepth);
        slope = found ? min(slope, delta) : delta;
        found = true;
    }
    return slope;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 tid : SV_DispatchThreadID)
{
    uint width, height;
    inputDiffuse.GetDimensions(width, height);
    int2 extent = int2(width, height);
    if (tid.x >= width || tid.y >= height)
        return;

    int2 pixel = int2(tid);
    float4 centerDiffuse = inputDiffuse.Load(int3(pixel, 0));
    float4 centerSpecular = inputSpecular.Load(int3(pixel, 0));
    if (pc.enabled == 0u)
    {
        outputDiffuse[tid] = centerDiffuse;
        outputSpecular[tid] = centerSpecular;
        return;
    }

    uint centerID = instanceID.Load(int3(pixel, 0));
    float centerDepth = linearDepth.Load(int3(pixel, 0));
    float4 centerNormalRoughness = normalRoughness.Load(int3(pixel, 0));
    if (centerID == ~0u || !(centerDepth > 0.0f) || isnan(centerDepth) || isinf(centerDepth) ||
        !IsFiniteRadiance(centerNormalRoughness.xyz) || dot(centerNormalRoughness.xyz, centerNormalRoughness.xyz) < 0.25f ||
        !IsFiniteRadiance(centerDiffuse.rgb) || !IsFiniteRadiance(centerSpecular.rgb))
    {
        // Background and visible emitters have invalid guides; never mix them with surfaces.
        outputDiffuse[tid] = centerDiffuse;
        outputSpecular[tid] = centerSpecular;
        return;
    }

    float3 centerNormal = normalize(centerNormalRoughness.xyz);
    float4 centerAlbedoMetallic = albedoMetallic.Load(int3(pixel, 0));
    float2 depthSlope = float2(
        EstimateDepthSlope(pixel, int2(1, 0), extent, centerID, centerDepth),
        EstimateDepthSlope(pixel, int2(0, 1), extent, centerID, centerDepth));
    float centerDiffuseLuma = Luminance(centerDiffuse.rgb);
    float centerSpecularLuma = Luminance(centerSpecular.rgb);
    // Negative roughness marks transmission. First-surface guides cannot track refraction,
    // and near-mirror reflections should not be blurred using the receiver's geometry.
    bool filterSpecular = centerNormalRoughness.w >= 0.1f;
    float roughness = saturate(centerNormalRoughness.w);
    float specularRadius = max(0.5f, 8.0f * roughness * roughness);
    float specularNormalPower = lerp(max(pc.normalPower, 128.0f), pc.normalPower, roughness);

    // Keep a nonzero center weight even when every neighboring sample is rejected.
    float3 diffuseSum = centerDiffuse.rgb * 36.0f;
    float3 specularSum = centerSpecular.rgb * 36.0f;
    float diffuseWeightSum = 36.0f;
    float specularWeightSum = 36.0f;
    for (int y = -2; y <= 2; ++y)
    {
        for (int x = -2; x <= 2; ++x)
        {
            if (x == 0 && y == 0)
                continue;
            int2 offset = int2(x, y) * int(pc.stepWidth);
            int2 samplePixel = pixel + offset;
            // Do not clamp: repeated edge texels would change the kernel normalization.
            if (any(samplePixel < 0) || any(samplePixel >= extent))
                continue;
            if (instanceID.Load(int3(samplePixel, 0)) != centerID)
                continue;
            float sampleDepth = linearDepth.Load(int3(samplePixel, 0));
            if (!(sampleDepth > 0.0f) || isnan(sampleDepth) || isinf(sampleDepth))
                continue;
            float4 sampleNormalRoughness = normalRoughness.Load(int3(samplePixel, 0));
            if (!IsFiniteRadiance(sampleNormalRoughness.xyz) ||
                dot(sampleNormalRoughness.xyz, sampleNormalRoughness.xyz) < 0.25f ||
                ((centerNormalRoughness.w < 0.0f) != (sampleNormalRoughness.w < 0.0f)))
                continue;
            float normalAgreement = saturate(dot(centerNormal, normalize(sampleNormalRoughness.xyz)));
            float4 sampleAlbedoMetallic = albedoMetallic.Load(int3(samplePixel, 0));
            float3 sampleDiffuse = inputDiffuse.Load(int3(samplePixel, 0)).rgb;
            float3 sampleSpecular = inputSpecular.Load(int3(samplePixel, 0)).rgb;
            if (!IsFiniteRadiance(sampleDiffuse) || !IsFiniteRadiance(sampleSpecular))
                continue;

            // Linear ray distance (not NDC depth), with a distance- and slope-aware tolerance.
            float depthScale = pc.relativeDepthSigma * centerDepth + dot(depthSlope, abs(float2(offset)));
            float depthWeight = exp(-abs(centerDepth - sampleDepth) / max(depthScale, 1e-6f));
            float albedoDifference = length(centerAlbedoMetallic.rgb - sampleAlbedoMetallic.rgb);
            float materialWeight = exp(-albedoDifference / pc.albedoSigma -
                                       16.0f * abs(centerAlbedoMetallic.a - sampleAlbedoMetallic.a));
            float kernelWeight = kKernel[x + 2] * kKernel[y + 2];
            float surfaceWeight = kernelWeight * depthWeight * materialWeight;
            float diffuseWeight = surfaceWeight * pow(normalAgreement, pc.normalPower) *
                LuminanceWeight(centerDiffuseLuma, Luminance(sampleDiffuse));
            diffuseSum += sampleDiffuse * diffuseWeight;
            diffuseWeightSum += diffuseWeight;

            if (filterSpecular && sampleNormalRoughness.w >= 0.1f)
            {
                float roughnessWeight = exp(-abs(roughness - sampleNormalRoughness.w) / 0.1f);
                float specularSpatialWeight = exp(-0.5f * dot(float2(offset), float2(offset)) /
                                                  (specularRadius * specularRadius));
                float specularWeight = surfaceWeight * pow(normalAgreement, specularNormalPower) *
                    roughnessWeight * specularSpatialWeight *
                    LuminanceWeight(centerSpecularLuma, Luminance(sampleSpecular));
                specularSum += sampleSpecular * specularWeight;
                specularWeightSum += specularWeight;
            }
        }
    }
    outputDiffuse[tid] = float4(diffuseSum / diffuseWeightSum, centerDiffuse.a);
    outputSpecular[tid] = filterSpecular
        ? float4(specularSum / specularWeightSum, centerSpecular.a)
        : centerSpecular;
}
