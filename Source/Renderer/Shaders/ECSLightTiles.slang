import ICommon;
import IGPUScene;
#include "LightTiles.h"

uniform UBO globalParams;
ByteAddressBuffer lights;
RWStructuredBuffer<uint2> lightTiles; // {total incident-light count, overflow flag}
RWStructuredBuffer<uint> tileLightIndices;

static const uint kThreadCount = kRasterLightTileWidth * kRasterLightTileWidth;
groupshared float4 tilePlanes[4]; // Inward-facing world-space side planes, with no depth bounds.
groupshared uint tileLightCount;
groupshared uint tileLightOverflow;
groupshared uint tileIndices[kRasterLightTileCapacity];

// Conservative point/spot influence sphere. Spot-cone culling remains a future optimization.
bool PointSpotIntersectsTile(GSLight light)
{
    float minIncidentLight = globalParams.rasterMinLightTileIncidentEnergy;
    if (minIncidentLight <= 0.0f)
        return true;
    float3 peak = light.color * light.power;
    float radius = sqrt(max(max(peak.x, peak.y), peak.z) / minIncidentLight);
    [unroll]
    for (uint plane = 0u; plane < 4u; plane++)
    {
        if (dot(tilePlanes[plane], float4(light.position, 1.0f)) < -radius)
            return false;
    }
    return true;
}

[shader("compute")]
[numthreads(kRasterLightTileWidth, kRasterLightTileWidth, 1)]
void main(uint2 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    uint2 extent = uint2(uint(globalParams.fbWidth), uint(globalParams.fbHeight));
    if (lane == 0u)
    {
        tileLightCount = 0u;
        tileLightOverflow = 0u;

        // Use pixel edges, not centers; clamp partial tiles at the framebuffer boundary.
        float2 uvMin = float2(group * kRasterLightTileWidth) / float2(extent);
        float2 uvMax = float2(min((group + 1u) * kRasterLightTileWidth, extent)) / float2(extent);
        float2 ndcMin = float2(uvMin.x * 2.0f - 1.0f, 1.0f - uvMax.y * 2.0f);
        float2 ndcMax = float2(uvMax.x * 2.0f - 1.0f, 1.0f - uvMin.y * 2.0f);

        // Transform clip-space half-spaces to world space. This also supports orthographic cameras.
        float4x4 planeTransform = transpose(mul(globalParams.proj, globalParams.view));
        tilePlanes[0] = mul(planeTransform, float4(1.0f, 0.0f, 0.0f, -ndcMin.x));
        tilePlanes[1] = mul(planeTransform, float4(-1.0f, 0.0f, 0.0f, ndcMax.x));
        tilePlanes[2] = mul(planeTransform, float4(0.0f, 1.0f, 0.0f, -ndcMin.y));
        tilePlanes[3] = mul(planeTransform, float4(0.0f, -1.0f, 0.0f, ndcMax.y));
        [unroll]
        for (uint plane = 0u; plane < 4u; plane++)
            tilePlanes[plane] /= length(tilePlanes[plane].xyz);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint lightIndex = lane; lightIndex < globalParams.lights.count; lightIndex += kThreadCount)
    {
        GSLight light = GSLoad<GSLight>(lights, globalParams.lights.offset + lightIndex);
        uint type = GSLightType(light);
        if (type == kGSLightTypeEnvironment || !any(light.color * light.power > 0.0f))
            continue;
        if ((type == kGSLightTypePoint || type == kGSLightTypeSpot) && !PointSpotIntersectsTile(light))
            continue;
        uint slot;
        InterlockedAdd(tileLightCount, 1u, slot);
        if (slot < kRasterLightTileCapacity)
            tileIndices[slot] = lightIndex;
        else
            InterlockedOr(tileLightOverflow, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    uint tilesX = (extent.x + kRasterLightTileWidth - 1u) / kRasterLightTileWidth;
    uint tile = group.y * tilesX + group.x;
    // Only publish initialized list entries. Overflow tiles are evaluated with the full light list in PS.
    for (uint index = lane; index < min(tileLightCount, kRasterLightTileCapacity); index += kThreadCount)
        tileLightIndices[tile * kRasterLightTileCapacity + index] = tileIndices[index];
    if (lane == 0u)
        lightTiles[tile] = uint2(tileLightCount, tileLightOverflow);
}
