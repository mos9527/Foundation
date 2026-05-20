import IFlags;
import IBindless3D;
import ICommon;
import IPostprocess;
[[SpecializationConstant]] int flags;

// Buffers (Welford online mean - already averaged)
Texture2D<float4> diffuseTex;
Texture2D<float4> specularTex;
SamplerState lutSampler;
uniform UBO globalParams;

uint GetPTPostprocessTileSide()
{
    return max(globalParams.ptDispatchTileSide, 1u);
}

uint GetPTPostprocessTileSampleCount()
{
    uint tileSide = GetPTPostprocessTileSide();
    return max(tileSide * tileSide, 1u);
}

uint PTPostprocessDistanceSquared(uint2 a, uint2 b)
{
    int2 d = int2(int(a.x) - int(b.x), int(a.y) - int(b.y));
    return uint(d.x * d.x + d.y * d.y);
}

uint2 GetPTPostprocessSourcePixel(uint2 pixel)
{
    uint tileSide = GetPTPostprocessTileSide();
    if (tileSide <= 1u)
        return pixel;

    uint tileSampleCount = GetPTPostprocessTileSampleCount();
    if (globalParams.ptAccumulatedFrames >= tileSampleCount - 1u)
        return pixel;

    uint completedTileProgress = globalParams.ptAccumulatedFrames + 1u;
    uint2 extent = uint2(uint(globalParams.fbWidth), uint(globalParams.fbHeight));
    uint2 tileOrigin = (pixel / tileSide) * tileSide;
    uint2 tileExtent = min(uint2(tileSide), extent - tileOrigin);
    uint2 tileOffset = pixel - tileOrigin;
    uint pixelRank = tileOffset.y * tileSide + tileOffset.x;
    if (pixelRank < completedTileProgress)
        return pixel;

    uint lastValidRank = (tileExtent.y - 1u) * tileSide + tileExtent.x;
    if (completedTileProgress >= lastValidRank)
        return pixel;

    uint completeRows = completedTileProgress / tileSide;
    uint partialColumns = completedTileProgress - completeRows * tileSide;
    uint writtenFullRows = min(completeRows, tileExtent.y);
    uint writtenPartialColumns = completeRows < tileExtent.y ? min(partialColumns, tileExtent.x) : 0u;

    uint2 sourceOffset = uint2(0u);
    uint bestDistance = ~0u;
    if (writtenFullRows > 0u)
    {
        uint2 candidate = uint2(min(tileOffset.x, tileExtent.x - 1u), min(tileOffset.y, writtenFullRows - 1u));
        sourceOffset = candidate;
        bestDistance = PTPostprocessDistanceSquared(tileOffset, candidate);
    }
    if (writtenPartialColumns > 0u)
    {
        uint2 candidate = uint2(min(tileOffset.x, writtenPartialColumns - 1u), completeRows);
        uint distance = PTPostprocessDistanceSquared(tileOffset, candidate);
        if (distance < bestDistance)
            sourceOffset = candidate;
    }

    return tileOrigin + sourceOffset;
}

float3 LoadPTPostprocessRadiance(int3 coord)
{
    if ((flags & kViewAOVDiffuse) != 0)
        return diffuseTex.Load(coord).xyz;
    if ((flags & kViewAOVSpecular) != 0)
        return specularTex.Load(coord).xyz;
    return diffuseTex.Load(coord).xyz + specularTex.Load(coord).xyz;
}

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0, float2 fragCoord : SV_Position) : SV_TARGET {
    uint2 pixel = uint2(uint(fragCoord.x), uint(fragCoord.y));
    uint2 sourcePixel = GetPTPostprocessSourcePixel(pixel);
    int3 coord = int3(int(sourcePixel.x), int(sourcePixel.y), 0);

    float3 src = LoadPTPostprocessRadiance(coord);
    if ((flags & kViewPTRayDX) != 0)
        return float4(saturate(src), 1.0f);

    PostprocessInfo pp;
    pp.Exposure = globalParams.camEV;
    return float4(Postprocess(src, pp, textures3D[NonUniformResourceIndex(globalParams.viewLutIndex)], lutSampler), 1.0f);
}