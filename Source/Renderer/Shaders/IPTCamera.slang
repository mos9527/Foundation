import IMath;

// Keep camera sampling shared with temporal reprojection: the realtime PCG sampler
// starts at (pixel, traced frame number), and this is its first sample2D().
public float2 PTSampleCameraPixel<S : ISampler>(uint2 pixel, inout S rng, bool debugView = false)
{
    float2 jitter = lerp(float2(-0.5f), float2(0.5f), rng.sample2D());
    float2 pixelSample = float2(pixel) + 0.5f;
    if (!debugView)
        pixelSample += jitter;
    return pixelSample;
}

public float2 PTCameraNDC(float2 pixelSample, float2 extent)
{
    float2 ndc = pixelSample / extent * 2.0f - 1.0f;
    ndc.y = -ndc.y;
    return ndc;
}

public float3 PTPinholeRayDirection(float2 pixelSample, float2 extent, float2 tanHalfFov, float4x4 inverseView)
{
    float2 ndc = PTCameraNDC(pixelSample, extent);
    float3 viewDirection = normalize(float3(ndc * tanHalfFov, -1.0f));
    return normalize(mul(inverseView, float4(viewDirection, 0.0f)).xyz);
}

// Returns previous-screen UV and signed view depth. Use the same FOV mapping as
// ray generation rather than a potentially raster-jittered projection matrix.
public float3 PTProjectPinhole(float3 worldPosition, float4x4 view, float2 tanHalfFov)
{
    float3 viewPosition = mul(view, float4(worldPosition, 1.0f)).xyz;
    float viewDepth = -viewPosition.z;
    float2 ndc = viewPosition.xy / (max(viewDepth, 1e-6f) * tanHalfFov);
    return float3(ndc * float2(0.5f, -0.5f) + 0.5f, viewDepth);
}
