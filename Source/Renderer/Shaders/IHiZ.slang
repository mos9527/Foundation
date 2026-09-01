public float SampleHiZConservative(Texture2D<float> texture, SamplerState sampler, float4 aabb,
                                   uint2 size, uint levels)
{
    int2 baseSize = int2(size);
    float4 rectUV = saturate(aabb);
    int2 rectMin = clamp(int2(floor(rectUV.xy * baseSize)), int2(0), baseSize - 1);
    int2 rectMax = clamp(int2(ceil(rectUV.zw * baseSize)) - 1, rectMin, baseSize - 1);

    int longestSpan = max(rectMax.x - rectMin.x, rectMax.y - rectMin.y);
    int mip = longestSpan > 0 ? max(int(floor(log2(float(longestSpan)))) - 1, 0) : 0;
    int2 mipMin = rectMin >> mip;
    int2 mipMax = rectMax >> mip;
    if (any(mipMax - mipMin > int2(3)))
        mip++;

    mip = min(mip, int(levels) - 1);
    mipMin = rectMin >> mip;
    mipMax = rectMax >> mip;
    int2 mipSize = max(baseSize >> mip, int2(1));

    float minDepth = 1.0f;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int2 pixel = min(mipMin + int2(x, y), mipMax);
            float2 uv = (float2(pixel) + 0.5f) / float2(mipSize);
            minDepth = min(minDepth, texture.SampleLevel(sampler, uv, float(mip)));
        }
    }
    return minDepth;
}
