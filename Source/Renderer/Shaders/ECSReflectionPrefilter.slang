import IMath;

Texture2D<float4> sourceRadiance;
SamplerState radianceSampler;
RWTexture2D<float4> outputRadiance;

struct ReflectionPrefilterPushConstants
{
    uint width;
    uint height;
    uint mip;
    uint sampleCount;
};
[[vk::push_constant]] ReflectionPrefilterPushConstants pc;

float2 Hammersley2D(uint i, uint count)
{
    return float2(float(i) / float(count), float(reversebits(i)) * 2.3283064365386963e-10f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    uint2 extent = uint2(pc.width, pc.height);
    if (any(pixel >= extent))
        return;
    // Preserve the lighting pass's exact half-resolution base for the sharp/full-res transition.
    if (pc.mip == 0u)
    {
        outputRadiance[pixel] = sourceRadiance.Load(int3(pixel, 0));
        return;
    }

    uint baseWidth, baseHeight, sourceMips;
    sourceRadiance.GetDimensions(0u, baseWidth, baseHeight, sourceMips);
    float2 baseExtent = float2(baseWidth, baseHeight);
    float2 uv = (float2(pixel) + 0.5f) / float2(extent);
    // Unlike an HDRI, screen radiance has no direction parameterization. Use a local,
    // front-facing image plane (V=N), with GGX lobe width indexed by projected pixels.
    // Level m represents diameter 2^(m+1) in full-res pixels, matching the reflection LOD.
    float radius = 0.5f * exp2(float(pc.mip)); // Half-res base texels
    float planeScale = 0.5f * max(baseExtent.x, baseExtent.y);
    float alpha = radius / (2.0f * planeScale); // Reflected lobe slope is approximately 2*alpha.
    float alpha2 = alpha * alpha;
    uint pairCount = max(pc.sampleCount / 2u, 1u);
    float3 filtered = float3(0.0f);
    float totalWeight = 0.0f;

    for (uint i = 0u; i < pairCount; ++i)
    {
        float2 e = Hammersley2D(i, pairCount);
        // Match the HDRI path's truncated GGX half-vector sampling. Opposite pairs make
        // the finite kernel symmetric without per-frame random rotation / temporal noise.
        float phi = Pi * e.x;
        float u = e.y * 0.995f;
        float unitSlope2 = u / (1.0f - u);
        float2 halfSlope = alpha * sqrt(unitSlope2) * float2(cos(phi), sin(phi));
        float slope2 = alpha2 * unitSlope2;
        // H=normalize(float3(halfSlope,1)), L=reflect(-N,H). Work in slopes to avoid
        // cancellation in 1-NoH^2 for the very narrow high-resolution GGX levels.
        float NoL = (1.0f - slope2) / (1.0f + slope2);
        if (NoL <= 1e-4f)
            continue;
        float2 offset = (2.0f * halfSlope / (1.0f - slope2)) * planeScale / baseExtent;

        // PDF(L)=D(H)/4 for V=N. Convert solid-angle density to base-image area
        // density using dOmega/dArea=NoL^3/planeScale^2 before choosing a source mip.
        float denom = 1.0f + unitSlope2;
        float pdfDirection = (1.0f + slope2) * (1.0f + slope2) /
                             (4.0f * Pi * alpha2 * denom * denom);
        float pdfPixels = pdfDirection * NoL * NoL * NoL / (planeScale * planeScale);
        float sampleArea = 1.0f / (float(2u * pairCount) * max(pdfPixels, 1e-12f));
        float sourceLOD = clamp(0.5f * log2(sampleArea), 0.0f, float(sourceMips - 1u));

        for (uint side = 0u; side < 2u; ++side)
        {
            float2 sampleUV = uv + (side == 0u ? offset : -offset);
            // There is no off-screen radiance: do not stretch edge texels across a broad lobe.
            // Renormalize the available taps; the reflection pass retains its environment fade.
            if (any(sampleUV < 0.0f) || any(sampleUV > 1.0f))
                continue;
            filtered += sourceRadiance.SampleLevel(radianceSampler, sampleUV, sourceLOD).rgb * NoL;
            totalWeight += NoL;
        }
    }

    // Split sum, part 1: normalized radiance only, as in PrefilterEnvmapSpecular.
    // Fresnel/visibility (part 2) are applied once by the existing GGX LUT in reflections.
    float3 result = totalWeight > 0.0f ? filtered / totalWeight :
                   sourceRadiance.SampleLevel(radianceSampler, uv, 0.0f).rgb;
    outputRadiance[pixel] = float4(result, 1.0f);
}
