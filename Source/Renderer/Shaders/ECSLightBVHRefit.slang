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

float SafeACos(float value)
{
    return acos(clamp(value, -1.0f, 1.0f));
}

float ComputeCosConeAngle(float3 coneDirection, float cosConeAngle, float3 otherDirection, float otherCosConeAngle)
{
    if (cosConeAngle == kLightBVHInvalidCosConeAngle || otherCosConeAngle == kLightBVHInvalidCosConeAngle)
        return kLightBVHInvalidCosConeAngle;

    float currentAngle = SafeACos(cosConeAngle);
    float otherAngle = SafeACos(dot(coneDirection, otherDirection)) + SafeACos(otherCosConeAngle);
    float resultAngle = max(currentAngle, otherAngle);
    return resultAngle < 3.14159265358979323846f ? cos(resultAngle) : kLightBVHInvalidCosConeAngle;
}

float3 ConeUnion(float3 aDirection, float aCosConeAngle, float3 bDirection, float bCosConeAngle,
                 out float cosConeAngle)
{
    float3 direction = aDirection + bDirection;
    if (aCosConeAngle == kLightBVHInvalidCosConeAngle || bCosConeAngle == kLightBVHInvalidCosConeAngle ||
        length(direction) < 1e-8f)
    {
        cosConeAngle = kLightBVHInvalidCosConeAngle;
        return float3(0.0f);
    }

    direction = normalize(direction);
    float aAngle = SafeACos(dot(direction, aDirection)) + SafeACos(aCosConeAngle);
    float bAngle = SafeACos(dot(direction, bDirection)) + SafeACos(bCosConeAngle);
    float resultAngle = max(aAngle, bAngle);
    if (resultAngle >= 3.14159265358979323846f)
    {
        cosConeAngle = kLightBVHInvalidCosConeAngle;
        return float3(0.0f);
    }
    cosConeAngle = cos(resultAngle);
    return direction;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void updateLeafNodes(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= pc.gNodeCount)
        return;
    uint localIndex = gNodeIndices[pc.gFirstNodeOffset + DTid.x];
    uint nodeIndex = globalParams.lightBVHNodes.offset + localIndex;
    GSLightBVHNode node = lightBVHNodes[nodeIndex];
    uint count = LightBVHNodeLightCount(node);
    uint offset = LightBVHNodeLightOffset(node);

    float3 aabbMin = float3(1e30f, 1e30f, 1e30f);
    float3 aabbMax = float3(-1e30f, -1e30f, -1e30f);
    float3 normalsSum = float3(0, 0, 0);
    float flux = 0.0f;
    for (uint i = 0; i < count; ++i)
    {
        uint lightIndex = lightBVHLightIndices[globalParams.lightBVHLightIndices.offset + offset + i];
        GSLight light = lights[globalParams.lights.offset + lightIndex];
        float3 cMin, cMax, coneDir;
        float cosCone;
        ComputeLightBounds(light, cMin, cMax, coneDir, cosCone);
        aabbMin = min(aabbMin, cMin);
        aabbMax = max(aabbMax, cMax);
        normalsSum += coneDir;
        flux += ComputeLightProposalWeight(light);
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
            uint lightIndex = lightBVHLightIndices[globalParams.lightBVHLightIndices.offset + offset + i];
            GSLight light = lights[globalParams.lights.offset + lightIndex];
            float3 cMin, cMax, coneDir;
            float cosCone;
            ComputeLightBounds(light, cMin, cMax, coneDir, cosCone);
            node.cosConeAngle = ComputeCosConeAngle(node.coneDirection, node.cosConeAngle, coneDir, cosCone);
        }
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
    uint nodeIndex = globalParams.lightBVHNodes.offset + localIndex;
    GSLightBVHNode node = lightBVHNodes[nodeIndex];
    uint leftIndex = nodeIndex + 1u;
    uint rightIndex = globalParams.lightBVHNodes.offset + LightBVHNodeRightChild(node);
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

    node.coneDirection =
        ConeUnion(left.coneDirection, left.cosConeAngle, right.coneDirection, right.cosConeAngle, node.cosConeAngle);
    lightBVHNodes[nodeIndex] = node;
}
