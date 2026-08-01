import ICommon;
import ILight;

public static const float kLightBVHInvalidCosConeAngle = -1.0f;
public static const uint kLightBVHLeafBit = 1u << 31;
public static const uint kLightBVHLightCountBits = 4u;
public static const uint kLightBVHLightOffsetBits = 31u - kLightBVHLightCountBits;

// Included after `uniform UBO globalParams` and `StructuredBuffer<GSLight> gLights` are declared.
UBO GetUBO()
{
    return globalParams;
}

GSLight GetLight(uint lightIndex)
{
    return gLights[GetUBO().lights.offset + lightIndex];
}

GSEmissiveCluster GetEmissiveCluster(uint clusterIndex)
{
    return gEmissiveClusters[GetUBO().emissiveClusters.offset + clusterIndex];
}

bool IsClusterSelection(uint selection)
{
    return (selection & kLightSelectionClusterBit) != 0u;
}

uint SelectionIndex(uint selection)
{
    return selection & kLightSelectionIndexMask;
}

float ComputeSelectionProposalWeight(uint selection)
{
    return IsClusterSelection(selection)
        ? GetEmissiveCluster(SelectionIndex(selection)).flux
        : ComputeLightProposalWeight(GetLight(selection));
}

uint SelectionEntitySlot(uint selection)
{
    return IsClusterSelection(selection) ? GetUBO().lights.count + SelectionIndex(selection) : selection;
}

public struct GSLightBVHNode
{
    public uint header;
    public float3 origin;
    public float3 extent;
    public float flux;
    public float3 coneDirection;
    public float cosConeAngle;
};

public bool LightBVHNodeIsLeaf(GSLightBVHNode node)
{
    return (node.header & kLightBVHLeafBit) != 0u;
}

public uint LightBVHNodeRightChild(GSLightBVHNode node)
{
    return node.header;
}

public uint LightBVHNodeLightCount(GSLightBVHNode node)
{
    return (node.header >> kLightBVHLightOffsetBits) & ((1u << kLightBVHLightCountBits) - 1u);
}

public uint LightBVHNodeLightOffset(GSLightBVHNode node)
{
    return node.header & ((1u << kLightBVHLightOffsetBits) - 1u);
}

StructuredBuffer<GSLightBVHNode> lightBVHNodes;
StructuredBuffer<uint> lightBVHLightIndices;
StructuredBuffer<uint2> lightBVHBitmasks;

float CosSubClamped(float sinThetaA, float cosThetaA, float sinThetaB, float cosThetaB)
{
    if (cosThetaA > cosThetaB)
        return 1.0f;
    return cosThetaA * cosThetaB + sinThetaA * sinThetaB;
}

float SinSubClamped(float sinThetaA, float cosThetaA, float sinThetaB, float cosThetaB)
{
    if (cosThetaA > cosThetaB)
        return 0.0f;
    return sinThetaA * cosThetaB - cosThetaA * sinThetaB;
}

void BoundSphereSubtendedConeAngle(float3 toCenter, float sqrRadius, out float sinTheta, out float cosTheta)
{
    float dist2 = max(dot(toCenter, toCenter), 1e-12f);
    if (dist2 <= sqrRadius)
    {
        sinTheta = 1.0f;
        cosTheta = 0.0f;
        return;
    }
    float sin2 = clamp(sqrRadius / dist2, 0.0f, 1.0f);
    sinTheta = sqrt(sin2);
    cosTheta = sqrt(max(0.0f, 1.0f - sin2));
}

float BoundCosineTerm(float3 posW, float3 normalW, float3 center, float3 extent, out float cosThetaCone)
{
    float sqrRadius = dot(extent, extent);
    float sinThetaCone;
    BoundSphereSubtendedConeAngle(center - posW, sqrRadius, sinThetaCone, cosThetaCone);
    float3 L = normalize(center - posW);
    float cosThetaL = clamp(dot(normalW, L), -1.0f, 1.0f);
    float sinThetaL = sqrt(max(0.0f, 1.0f - cosThetaL * cosThetaL));
    return saturate(CosSubClamped(sinThetaL, cosThetaL, sinThetaCone, cosThetaCone));
}

float ComputeNodeImportance(float3 posW, float3 normalW, bool upperHemisphere, GSLightBVHNode node)
{
    float flux = node.flux;
    float distance = length(node.origin - posW);
    float cosThetaBoundingCone = 0.0f;
    float NdotL = BoundCosineTerm(posW, normalW, node.origin, node.extent, cosThetaBoundingCone);
    if (!upperHemisphere)
        NdotL = 1.0f;

    float orientationWeight = 1.0f;
    float cosConeAngle = node.cosConeAngle;
    if (cosConeAngle != kLightBVHInvalidCosConeAngle && cosConeAngle > 0.0f && distance > 1e-6f)
    {
        float3 dirToAabb = (node.origin - posW) / distance;
        float sinConeAngle = sqrt(max(0.0f, 1.0f - cosConeAngle * cosConeAngle));
        float cosTheta = dot(node.coneDirection, -dirToAabb);
        float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
        float sinThetaBoundingCone = sqrt(max(0.0f, 1.0f - cosThetaBoundingCone * cosThetaBoundingCone));
        float cosTheta0 = CosSubClamped(sinTheta, cosTheta, sinConeAngle, cosConeAngle);
        float sinTheta0 = SinSubClamped(sinTheta, cosTheta, sinConeAngle, cosConeAngle);
        float cosThetaPrime = CosSubClamped(sinTheta0, cosTheta0, sinThetaBoundingCone, cosThetaBoundingCone);
        orientationWeight = max(0.0f, cosThetaPrime);
    }

    float halfRadius = max(node.extent.x, max(node.extent.y, node.extent.z));
    distance = max(halfRadius, distance);
    return (flux * NdotL) * orientationWeight / (distance * distance);
}

float ComputeFiniteRootImportance(float3 posW, float3 normalW, bool upperHemisphere)
{
    if (GetUBO().lightBVHValid == 0u || GetUBO().lightBVHNodes.count == 0u)
        return 0.0f;
    GSLightBVHNode root = lightBVHNodes[GetUBO().lightBVHNodes.offset];
    return ComputeNodeImportance(posW, normalW, upperHemisphere, root);
}

float ComputeDistantNodeImportance(float3 posW, float3 normalW, bool upperHemisphere, GSLightBVHNode node)
{
    float flux = node.flux;
    float NdotL = 1.0f;
    float cosThetaCone = node.cosConeAngle;
    
    if (cosThetaCone != kLightBVHInvalidCosConeAngle && cosThetaCone > 0.0f)
    {
        float cosThetaL = clamp(dot(normalW, node.coneDirection), -1.0f, 1.0f);
        float sinThetaL = sqrt(max(0.0f, 1.0f - cosThetaL * cosThetaL));
        float sinThetaCone = sqrt(max(0.0f, 1.0f - cosThetaCone * cosThetaCone));
        NdotL = saturate(CosSubClamped(sinThetaL, cosThetaL, sinThetaCone, cosThetaCone));
    }
    
    if (!upperHemisphere)
        NdotL = 1.0f; // Disable culling for volumes / non-surface

    return flux * NdotL;
}

float ComputeDistantRootImportance(float3 posW, float3 normalW, bool upperHemisphere)
{
    if (GetUBO().lightBVHDistantNodes.count == 0u)
        return 0.0f;
    return ComputeDistantNodeImportance(posW, normalW, upperHemisphere,
                                        lightBVHNodes[GetUBO().lightBVHDistantNodes.offset]);
}

float EvalMixtureTotal(float3 posW, float3 normalW, bool upperHemisphere, out float finiteWeight,
                       out float distantWeight)
{
    finiteWeight = ComputeFiniteRootImportance(posW, normalW, upperHemisphere);
    distantWeight = ComputeDistantRootImportance(posW, normalW, upperHemisphere);
    return finiteWeight + distantWeight;
}

bool TraverseLightBVH(float3 posW, float3 normalW, bool upperHemisphere, inout float u, out float pdf,
                      out uint leafNodeIndex)
{
    pdf = 1.0f;
    leafNodeIndex = GetUBO().lightBVHNodes.offset;
    GSLightBVHNode node = lightBVHNodes[leafNodeIndex];
    while (!LightBVHNodeIsLeaf(node))
    {
        uint leftIndex = leafNodeIndex + 1u;
        uint rightIndex = GetUBO().lightBVHNodes.offset + LightBVHNodeRightChild(node);
        float leftImp = ComputeNodeImportance(posW, normalW, upperHemisphere, lightBVHNodes[leftIndex]);
        float rightImp = ComputeNodeImportance(posW, normalW, upperHemisphere, lightBVHNodes[rightIndex]);
        float sum = leftImp + rightImp;
        if (sum <= 0.0f)
            return false;
        float pLeft = leftImp / sum;
        if (u < pLeft)
        {
            u = u / pLeft;
            pdf *= pLeft;
            leafNodeIndex = leftIndex;
        }
        else
        {
            float pRight = 1.0f - pLeft;
            u = (u - pLeft) / pRight;
            pdf *= pRight;
            leafNodeIndex = rightIndex;
        }
        node = lightBVHNodes[leafNodeIndex];
    }
    return true;
}

bool TraverseDistantLightBVH(float3 posW, float3 normalW, bool upperHemisphere, inout float u, out float pdf, out uint leafNodeIndex)
{
    pdf = 1.0f;
    leafNodeIndex = GetUBO().lightBVHDistantNodes.offset;
    GSLightBVHNode node = lightBVHNodes[leafNodeIndex];
    while (!LightBVHNodeIsLeaf(node))
    {
        uint leftIndex = leafNodeIndex + 1u;
        uint rightIndex = GetUBO().lightBVHNodes.offset + LightBVHNodeRightChild(node);
        float leftImp = ComputeDistantNodeImportance(posW, normalW, upperHemisphere, lightBVHNodes[leftIndex]);
        float rightImp = ComputeDistantNodeImportance(posW, normalW, upperHemisphere, lightBVHNodes[rightIndex]);
        float sum = leftImp + rightImp;
        if (sum <= 0.0f)
            return false;
        float pLeft = leftImp / sum;
        if (u < pLeft)
        {
            u /= pLeft;
            pdf *= pLeft;
            leafNodeIndex = leftIndex;
        }
        else
        {
            float pRight = 1.0f - pLeft;
            u = (u - pLeft) / pRight;
            pdf *= pRight;
            leafNodeIndex = rightIndex;
        }
        node = lightBVHNodes[leafNodeIndex];
    }
    return true;
}

bool PickLeafLight(uint leafNodeIndex, float u, out float pdf, out uint lightIndex)
{
    GSLightBVHNode node = lightBVHNodes[leafNodeIndex];
    uint count = LightBVHNodeLightCount(node);
    uint offset = LightBVHNodeLightOffset(node);
    if (count == 0u)
        return false;

    float total = 0.0f;
    for (uint i = 0; i < count; ++i)
    {
        uint idx = lightBVHLightIndices[GetUBO().lightBVHLightIndices.offset + offset + i];
        total += ComputeSelectionProposalWeight(idx);
    }
    if (total <= 0.0f)
        return false;

    float uScaled = u * total;
    float cdf = 0.0f;
    for (uint i = 0; i < count; ++i)
    {
        uint idx = lightBVHLightIndices[GetUBO().lightBVHLightIndices.offset + offset + i];
        float weight = ComputeSelectionProposalWeight(idx);
        cdf += weight;
        if (uScaled < cdf || i + 1u == count)
        {
            lightIndex = idx;
            pdf = weight / total;
            return pdf > 0.0f;
        }
    }
    return true;
}

float EvalLeafLightPdf(uint leafNodeIndex, uint lightIndex)
{
    GSLightBVHNode node = lightBVHNodes[leafNodeIndex];
    uint count = LightBVHNodeLightCount(node);
    uint offset = LightBVHNodeLightOffset(node);
    float total = 0.0f;
    float selected = 0.0f;
    for (uint i = 0; i < count; ++i)
    {
        uint idx = lightBVHLightIndices[GetUBO().lightBVHLightIndices.offset + offset + i];
        float weight = ComputeSelectionProposalWeight(idx);
        total += weight;
        if (idx == lightIndex)
            selected = weight;
    }
    return total > 0.0f ? selected / total : 0.0f;
}

float EvalFiniteTraversalPdf(float3 posW, float3 normalW, bool upperHemisphere, uint lightIndex, out uint leafNodeIndex)
{
    uint2 bits2 = lightBVHBitmasks[GetUBO().firstLightBVHBitmask + SelectionEntitySlot(lightIndex)];
    uint64_t bitmask = ((uint64_t)bits2.y << 32) | bits2.x;
    float pdf = 1.0f;
    leafNodeIndex = GetUBO().lightBVHNodes.offset;
    GSLightBVHNode node = lightBVHNodes[leafNodeIndex];
    while (!LightBVHNodeIsLeaf(node))
    {
        uint leftIndex = leafNodeIndex + 1u;
        uint rightIndex = GetUBO().lightBVHNodes.offset + LightBVHNodeRightChild(node);
        float leftImp = ComputeNodeImportance(posW, normalW, upperHemisphere, lightBVHNodes[leftIndex]);
        float rightImp = ComputeNodeImportance(posW, normalW, upperHemisphere, lightBVHNodes[rightIndex]);
        float sum = leftImp + rightImp;
        if (sum <= 0.0f)
            return 0.0f;
        float pLeft = leftImp / sum;
        bool chooseLeft = (bitmask & 1ull) == 0ull;
        if (chooseLeft)
        {
            pdf *= pLeft;
            leafNodeIndex = leftIndex;
        }
        else
        {
            pdf *= (1.0f - pLeft);
            leafNodeIndex = rightIndex;
        }
        bitmask >>= 1;
        node = lightBVHNodes[leafNodeIndex];
    }
    return pdf;
}

float EvalDistantTraversalPdf(float3 posW, float3 normalW, bool upperHemisphere, uint lightIndex, out uint leafNodeIndex)
{
    uint2 bits2 = lightBVHBitmasks[GetUBO().firstLightBVHBitmask + lightIndex];
    uint64_t bitmask = ((uint64_t)bits2.y << 32) | bits2.x;
    float pdf = 1.0f;
    leafNodeIndex = GetUBO().lightBVHDistantNodes.offset;
    GSLightBVHNode node = lightBVHNodes[leafNodeIndex];
    while (!LightBVHNodeIsLeaf(node))
    {
        uint leftIndex = leafNodeIndex + 1u;
        uint rightIndex = GetUBO().lightBVHNodes.offset + LightBVHNodeRightChild(node);
        float leftImp = ComputeDistantNodeImportance(posW, normalW, upperHemisphere, lightBVHNodes[leftIndex]);
        float rightImp = ComputeDistantNodeImportance(posW, normalW, upperHemisphere, lightBVHNodes[rightIndex]);
        float sum = leftImp + rightImp;
        if (sum <= 0.0f)
            return 0.0f;
        float pLeft = leftImp / sum;
        bool chooseLeft = (bitmask & 1ull) == 0ull;
        pdf *= chooseLeft ? pLeft : (1.0f - pLeft);
        leafNodeIndex = chooseLeft ? leftIndex : rightIndex;
        bitmask >>= 1;
        node = lightBVHNodes[leafNodeIndex];
    }
    return pdf;
}

public float EvalLightSelectionPMF(LightSampleContext ctx, uint lightIndex, bool useUniform)
{
    uint index = SelectionIndex(lightIndex);
    bool isCluster = IsClusterSelection(lightIndex);
    if ((!isCluster && index >= GetUBO().lights.count) ||
        (isCluster && index >= GetUBO().emissiveClusters.count))
        return 0.0f;

    if (useUniform)
        return 1.0f / float(GetUBO().lights.count + GetUBO().emissiveClusters.count);

    float3 posW = ctx.p();
    float3 normalW = ctx.ns;
    bool upperHemisphere = ctx.upperHemisphere;
    float finiteWeight;
    float distantWeight;
    float total = EvalMixtureTotal(posW, normalW, upperHemisphere, finiteWeight, distantWeight);
    if (total <= 0.0f)
        return 0.0f;

    uint type = isCluster ? ~0u : GSLightType(GetLight(index));
    if (!isCluster && (type == kGSLightTypeDirectional || type == kGSLightTypeEnvironment))
    {
        if (distantWeight <= 0.0f || GetUBO().lightBVHDistantNodes.count == 0u)
            return 0.0f;
        uint leafNodeIndex;
        float traversalPdf = EvalDistantTraversalPdf(posW, normalW, upperHemisphere, index, leafNodeIndex);
        return (distantWeight / total) * traversalPdf * EvalLeafLightPdf(leafNodeIndex, lightIndex);
    }

    if (finiteWeight <= 0.0f || GetUBO().lightBVHValid == 0u)
        return 0.0f;

    uint leafNodeIndex;
    float traversalPdf = EvalFiniteTraversalPdf(posW, normalW, upperHemisphere, lightIndex, leafNodeIndex);
    if (traversalPdf <= 0.0f)
        return 0.0f;
    float leafPdf = EvalLeafLightPdf(leafNodeIndex, lightIndex);
    return (finiteWeight / total) * traversalPdf * leafPdf;
}

public bool SampleLightSelection(LightSampleContext ctx, float u, out uint lightIndex, out float selectionPMF, bool useUniform)
{
    lightIndex = ~0u;
    selectionPMF = 0.0f;
    if (GetUBO().lights.count + GetUBO().emissiveClusters.count == 0u)
        return false;

    if (useUniform)
    {
        uint analyticCount = GetUBO().lights.count;
        uint n = analyticCount + GetUBO().emissiveClusters.count;
        uint entitySlot = min(uint(u * float(n)), n - 1u);
        lightIndex = entitySlot < analyticCount ? entitySlot : kLightSelectionClusterBit | (entitySlot - analyticCount);
        selectionPMF = 1.0f / float(n);
        return true;
    }

    float3 posW = ctx.p();
    float3 normalW = ctx.ns;
    bool upperHemisphere = ctx.upperHemisphere;
    float finiteWeight;
    float distantWeight;
    float total = EvalMixtureTotal(posW, normalW, upperHemisphere, finiteWeight, distantWeight);
    if (total <= 0.0f)
        return false;

    float uScaled = u * total;
    // Distant light path
    if (distantWeight > 0.0f && uScaled < distantWeight)
    {
        float uDistant = saturate(uScaled / distantWeight);
        float traversalPdf;
        uint leafNodeIndex;
        if (!TraverseDistantLightBVH(posW, normalW, upperHemisphere, uDistant, traversalPdf, leafNodeIndex))
            return false;
        float pickPdf;
        if (!PickLeafLight(leafNodeIndex, uDistant, pickPdf, lightIndex))
            return false;
        selectionPMF = (distantWeight / total) * traversalPdf * pickPdf;
        return selectionPMF > 0.0f;
    }

    // Finite light path
    if (finiteWeight <= 0.0f || GetUBO().lightBVHValid == 0u)
        return false;

    float uFinite = (uScaled - distantWeight) / finiteWeight;
    uFinite = saturate(uFinite);
    float leafPdf;
    uint leafNodeIndex;
    if (!TraverseLightBVH(posW, normalW, upperHemisphere, uFinite, leafPdf, leafNodeIndex))
        return false;
    float pickPdf;
    if (!PickLeafLight(leafNodeIndex, uFinite, pickPdf, lightIndex))
        return false;
    selectionPMF = (finiteWeight / total) * leafPdf * pickPdf;
    return selectionPMF > 0.0f;
}
