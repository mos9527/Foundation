#include "ICommon.slang"

static const float kLightBVHInvalidCosConeAngle = -1.0f;
static const uint kLightBVHLeafBit = 1u << 31;
static const uint kLightBVHLightCountBits = 4u;
static const uint kLightBVHLightOffsetBits = 31u - kLightBVHLightCountBits;

struct GSLightBVHNode
{
    uint header;
    float3 origin;
    float3 extent;
    float flux;
    float3 coneDirection;
    float cosConeAngle;
};

bool LightBVHNodeIsLeaf(GSLightBVHNode node)
{
    return (node.header & kLightBVHLeafBit) != 0u;
}

uint LightBVHNodeRightChild(GSLightBVHNode node)
{
    return node.header;
}

uint LightBVHNodeLightCount(GSLightBVHNode node)
{
    return (node.header >> kLightBVHLightOffsetBits) & ((1u << kLightBVHLightCountBits) - 1u);
}

uint LightBVHNodeLightOffset(GSLightBVHNode node)
{
    return node.header & ((1u << kLightBVHLightOffsetBits) - 1u);
}

struct PushConstant
{
    uint gFirstNodeOffset;
    uint gNodeCount;
};
[[vk::push_constant]] PushConstant pc;

uniform UBO globalParams;
StructuredBuffer<GSLight> lights;
RWStructuredBuffer<GSLightBVHNode> lightBVHNodes;
StructuredBuffer<uint> lightBVHLightIndices;
StructuredBuffer<uint> gNodeIndices;

float3 SafeNormalize(float3 v)
{
    float len = length(v);
    return len > 1e-8f ? v / len : float3(0, 0, 0);
}

void ComputeLightBounds(GSLight light, out float3 aabbMin, out float3 aabbMax, out float3 coneDirection,
                        out float cosConeAngle)
{
    uint type = GSLightType(light);
    coneDirection = float3(0, 0, 0);
    cosConeAngle = kLightBVHInvalidCosConeAngle;
    if (type == kGSLightTypePoint)
    {
        float radius = max(light.params.x, 1e-3f);
        aabbMin = light.position - float3(radius, radius, radius);
        aabbMax = light.position + float3(radius, radius, radius);
        return;
    }
    if (type == kGSLightTypeSpot)
    {
        float radius = max(light.params.x, 1e-3f);
        aabbMin = light.position - float3(radius, radius, radius);
        aabbMax = light.position + float3(radius, radius, radius);
        coneDirection = SafeNormalize(light.direction);
        cosConeAngle = clamp(light.params.z, -1.0f, 1.0f);
        return;
    }
    if (type == kGSLightTypeDisk)
    {
        float3 u = light.dpdu * light.params.x;
        float3 v = light.dpdv * light.params.y;
        float3 c0 = light.position + u + v;
        float3 c1 = light.position + u - v;
        float3 c2 = light.position - u + v;
        float3 c3 = light.position - u - v;
        aabbMin = min(min(c0, c1), min(c2, c3));
        aabbMax = max(max(c0, c1), max(c2, c3));
        if ((light.flags & kGSLightFlagTwoSided) == 0u)
        {
            coneDirection = SafeNormalize(light.direction);
            cosConeAngle = 1.0f;
        }
        return;
    }
    if (type == kGSLightTypeRect)
    {
        float3 c0 = light.position + light.dpdu + light.dpdv;
        float3 c1 = light.position + light.dpdu - light.dpdv;
        float3 c2 = light.position - light.dpdu + light.dpdv;
        float3 c3 = light.position - light.dpdu - light.dpdv;
        aabbMin = min(min(c0, c1), min(c2, c3));
        aabbMax = max(max(c0, c1), max(c2, c3));
        if ((light.flags & kGSLightFlagTwoSided) == 0u)
        {
            coneDirection = SafeNormalize(light.direction);
            cosConeAngle = 1.0f;
        }
        return;
    }
    aabbMin = float3(0, 0, 0);
    aabbMax = float3(0, 0, 0);
}

float SinFromCos(float cosAngle)
{
    return sqrt(max(0.0f, 1.0f - cosAngle * cosAngle));
}

[shader("compute")]
[numthreads(256, 1, 1)]
void updateLeafNodes(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= pc.gNodeCount)
        return;
    uint localIndex = gNodeIndices[pc.gFirstNodeOffset + DTid.x];
    uint nodeIndex = globalParams.firstLightBVHNode + localIndex;
    GSLightBVHNode node = lightBVHNodes[nodeIndex];
    uint count = LightBVHNodeLightCount(node);
    uint offset = LightBVHNodeLightOffset(node);

    float3 aabbMin = float3(1e30f, 1e30f, 1e30f);
    float3 aabbMax = float3(-1e30f, -1e30f, -1e30f);
    float3 normalsSum = float3(0, 0, 0);
    float flux = 0.0f;
    for (uint i = 0; i < count; ++i)
    {
        uint lightIndex = lightBVHLightIndices[globalParams.firstLightBVHLightIndex + offset + i];
        GSLight light = lights[globalParams.firstLight + lightIndex];
        float3 cMin, cMax, coneDir;
        float cosCone;
        ComputeLightBounds(light, cMin, cMax, coneDir, cosCone);
        aabbMin = min(aabbMin, cMin);
        aabbMax = max(aabbMax, cMax);
        normalsSum += coneDir;
        flux += max(0.0f, light.importance);
    }

    node.origin = (aabbMax + aabbMin) * 0.5f;
    node.extent = (aabbMax - aabbMin) * 0.5f;
    node.flux = flux;
    float coneLen = length(normalsSum);
    if (coneLen >= 1e-8f)
    {
        node.coneDirection = normalsSum / coneLen;
        node.cosConeAngle = 1.0f;
        for (uint i = 0; i < count; ++i)
        {
            uint lightIndex = lightBVHLightIndices[globalParams.firstLightBVHLightIndex + offset + i];
            GSLight light = lights[globalParams.firstLight + lightIndex];
            float3 cMin, cMax, coneDir;
            float cosCone;
            ComputeLightBounds(light, cMin, cMax, coneDir, cosCone);
            if (cosCone != kLightBVHInvalidCosConeAngle)
                node.cosConeAngle = min(node.cosConeAngle, dot(node.coneDirection, coneDir));
        }
        node.cosConeAngle = max(node.cosConeAngle, -1.0f);
    }
    else
    {
        node.coneDirection = float3(0, 0, 0);
        node.cosConeAngle = kLightBVHInvalidCosConeAngle;
    }
    lightBVHNodes[nodeIndex] = node;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void updateInternalNodes(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= pc.gNodeCount)
        return;
    uint localIndex = gNodeIndices[pc.gFirstNodeOffset + DTid.x];
    uint nodeIndex = globalParams.firstLightBVHNode + localIndex;
    GSLightBVHNode node = lightBVHNodes[nodeIndex];
    uint leftIndex = nodeIndex + 1u;
    uint rightIndex = globalParams.firstLightBVHNode + LightBVHNodeRightChild(node);
    GSLightBVHNode left = lightBVHNodes[leftIndex];
    GSLightBVHNode right = lightBVHNodes[rightIndex];

    float3 leftMin = left.origin - left.extent;
    float3 leftMax = left.origin + left.extent;
    float3 rightMin = right.origin - right.extent;
    float3 rightMax = right.origin + right.extent;
    float3 aabbMin = min(leftMin, rightMin);
    float3 aabbMax = max(leftMax, rightMax);
    node.origin = (aabbMax + aabbMin) * 0.5f;
    node.extent = (aabbMax - aabbMin) * 0.5f;
    node.flux = left.flux + right.flux;

    float3 coneSum = left.coneDirection + right.coneDirection;
    float coneLen = length(coneSum);
    node.cosConeAngle = kLightBVHInvalidCosConeAngle;
    node.coneDirection = float3(0, 0, 0);
    if (coneLen >= 1e-8f && left.cosConeAngle != kLightBVHInvalidCosConeAngle &&
        right.cosConeAngle != kLightBVHInvalidCosConeAngle)
    {
        float3 coneDirection = coneSum / coneLen;
        float cosLeftDiff = dot(coneDirection, left.coneDirection);
        float sinLeftDiff = SinFromCos(cosLeftDiff);
        float cosRightDiff = dot(coneDirection, right.coneDirection);
        float sinRightDiff = SinFromCos(cosRightDiff);
        float sinLeftCone = SinFromCos(left.cosConeAngle);
        float sinRightCone = SinFromCos(right.cosConeAngle);
        float sinLeftTotal = sinLeftCone * cosLeftDiff + sinLeftDiff * left.cosConeAngle;
        float sinRightTotal = sinRightCone * cosRightDiff + sinRightDiff * right.cosConeAngle;
        if (sinLeftTotal > 0.0f && sinRightTotal > 0.0f)
        {
            float cosLeftTotal = left.cosConeAngle * cosLeftDiff - sinLeftCone * sinLeftDiff;
            float cosRightTotal = right.cosConeAngle * cosRightDiff - sinRightCone * sinRightDiff;
            node.cosConeAngle = max(min(cosLeftTotal, cosRightTotal), -1.0f);
            node.coneDirection = coneDirection;
        }
    }
    lightBVHNodes[nodeIndex] = node;
}
