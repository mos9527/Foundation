// Raster coordinates: top-left UV origin, pixel centers, reverse-Z device depth.
public float3 ReconstructScreenPosition(uint2 pixel, float deviceDepth, float2 extent,
                                       float4x4 inverseViewProj)
{
    float2 uv = (float2(pixel) + 0.5f) / extent;
    float4 world = mul(inverseViewProj, float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f),
                                             deviceDepth, 1.0f));
    return world.xyz / world.w;
}

public bool ProjectScreenPosition(float3 position, float4x4 view, float4x4 proj,
                                  out float2 uv, out float clipW)
{
    float4 clip = mul(proj, mul(view, float4(position, 1.0f)));
    clipW = clip.w;
    uv = float2(0.0f);
    if (clip.w <= 0.0f || clip.z <= 0.0f || clip.z > clip.w)
        return false;
    uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
    return all(uv >= 0.0f) && all(uv < 1.0f);
}
