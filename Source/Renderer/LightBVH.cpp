#include "LightBVH.hpp"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <numbers>
#include <stack>
#include <vector>

namespace
{
struct AABB
{
    float3 minPoint{std::numeric_limits<float>::max()};
    float3 maxPoint{std::numeric_limits<float>::lowest()};

    bool valid() const { return all(lessThanEqual(minPoint, maxPoint)); }
    float3 center() const { return (minPoint + maxPoint) * 0.5f; }
    float3 extent() const { return maxPoint - minPoint; }
    float area() const
    {
        float3 d = extent();
        return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
    }

    AABB& operator|=(float3 const& p)
    {
        minPoint = min(minPoint, p);
        maxPoint = max(maxPoint, p);
        return *this;
    }

    AABB& operator|=(AABB const& other)
    {
        if (!other.valid())
            return *this;
        minPoint = min(minPoint, other.minPoint);
        maxPoint = max(maxPoint, other.maxPoint);
        return *this;
    }
};

struct Range
{
    uint32_t begin = 0;
    uint32_t end = 0;
    uint32_t middle() const { return (begin + end) / 2u; }
    uint32_t length() const { return end - begin; }
};

struct SplitResult
{
    uint32_t axis = ~0u;
    uint32_t lightIndex = ~0u;
    bool isValid() const { return axis != ~0u && lightIndex != ~0u; }
};

struct LightSortData
{
    AABB bounds;
    float3 center{};
    float3 coneDirection{};
    float cosConeAngle = 1.0f;
    float flux = 0.0f;
    uint32_t lightIndex = ~0u;
};

struct BuildingData
{
    Allocator* alloc;
    Vector<GSLightBVHNode>& nodes;
    Vector<LightSortData> lightsData;
    Vector<uint32_t> lightIndices;
    Vector<uint64_t> lightBitmasks;
    float currentNodeFlux = 0.0f;

    BuildingData(Vector<GSLightBVHNode>& bvhNodes, Allocator* allocator) :
        alloc(allocator), nodes(bvhNodes), lightsData(allocator), lightIndices(allocator), lightBitmasks(allocator)
    {
    }
};

float safeACos(float v)
{
    return std::acos(std::clamp(v, -1.0f, 1.0f));
}

float sinFromCos(float cosAngle)
{
    return std::sqrt(std::max(0.0f, 1.0f - cosAngle * cosAngle));
}

float computeCosConeAngle(float3 const& coneDir, float cosTheta, float3 const& otherConeDir, float cosOtherTheta)
{
    float cosResult = kLightBVHInvalidCosConeAngle;
    if (cosTheta != kLightBVHInvalidCosConeAngle && cosOtherTheta != kLightBVHInvalidCosConeAngle)
    {
        float const cosDiffTheta = dot(coneDir, otherConeDir);
        float const sinDiffTheta = sinFromCos(cosDiffTheta);
        float const sinOtherTheta = sinFromCos(cosOtherTheta);
        float const cosTotalTheta = cosOtherTheta * cosDiffTheta - sinOtherTheta * sinDiffTheta;
        float const sinTotalTheta = sinOtherTheta * cosDiffTheta + cosOtherTheta * sinDiffTheta;
        if (sinTotalTheta > 0.0f)
            cosResult = std::min(cosTheta, cosTotalTheta);
    }
    return cosResult;
}

float3 coneUnionOld(float3 aDir, float aCosTheta, float3 bDir, float bCosTheta, float& cosResult)
{
    float3 dir = aDir + bDir;
    if (aCosTheta == kLightBVHInvalidCosConeAngle || bCosTheta == kLightBVHInvalidCosConeAngle ||
        all(equal(dir, float3(0.0f))))
    {
        cosResult = kLightBVHInvalidCosConeAngle;
        return float3(0.0f);
    }

    dir = normalize(dir);
    float const aDiff = safeACos(dot(dir, aDir));
    float const bDiff = safeACos(dot(dir, bDir));
    cosResult = std::cos(std::max(aDiff + std::acos(aCosTheta), bDiff + std::acos(bCosTheta)));
    return dir;
}

float aabbVolume(AABB const& bb, float epsilon)
{
    if (!bb.valid())
        return -std::numeric_limits<float>::infinity();
    float3 dims = max(float3(epsilon), bb.extent());
    return dims.x * dims.y * dims.z;
}

float evalSAH(AABB const& bounds, uint32_t lightCount, LightBVHOptions const& parameters)
{
    float aabbCost = bounds.valid() ? (parameters.useVolumeOverSA ? aabbVolume(bounds, parameters.volumeEpsilon)
                                                                  : bounds.area())
                                    : 0.0f;
    return aabbCost * static_cast<float>(lightCount);
}

float computeOrientationCost(float theta_o)
{
    float theta_w = std::min(theta_o + std::numbers::pi_v<float> * 0.5f, std::numbers::pi_v<float>);
    float sin_theta_o = std::sin(theta_o);
    float cos_theta_o = std::cos(theta_o);
    return 2.0f * std::numbers::pi_v<float> * (1.0f - cos_theta_o) +
           0.5f * std::numbers::pi_v<float> *
               (2.0f * theta_w * sin_theta_o - std::cos(theta_o - 2.0f * theta_w) - 2.0f * theta_o * sin_theta_o +
                cos_theta_o);
}

float evalSAOH(AABB const& bounds, float flux, float cosTheta, LightBVHOptions const& parameters)
{
    float fluxCost = parameters.usePreintegration ? flux : 1.0f;
    float aabbCost = bounds.valid() ? (parameters.useVolumeOverSA ? aabbVolume(bounds, parameters.volumeEpsilon)
                                                                  : bounds.area())
                                    : 0.0f;
    float theta = cosTheta != kLightBVHInvalidCosConeAngle ? safeACos(cosTheta) : std::numbers::pi_v<float>;
    float orientationCost = parameters.useLightingCones ? computeOrientationCost(theta) : 1.0f;
    return fluxCost * aabbCost * orientationCost;
}

float3 computeLightingCone(Range const& lightRange, BuildingData const& data, float& cosTheta)
{
    float3 coneDirection{};
    cosTheta = kLightBVHInvalidCosConeAngle;

    float3 coneDirectionSum{};
    for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
        coneDirectionSum += data.lightsData[i].coneDirection;

    if (length(coneDirectionSum) >= FLT_MIN)
    {
        coneDirection = normalize(coneDirectionSum);
        cosTheta = 1.0f;
        for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
        {
            LightSortData const& ld = data.lightsData[i];
            cosTheta = computeCosConeAngle(coneDirection, cosTheta, ld.coneDirection, ld.cosConeAngle);
        }
    }
    return coneDirection;
}

float3 computeLightingConesInternal(uint32_t nodeIndex, BuildingData& data, float& cosConeAngle)
{
    if (!GSLightBVHNodeIsLeaf(data.nodes[nodeIndex]))
    {
        uint32_t leftIndex = nodeIndex + 1u;
        uint32_t rightIndex = GSLightBVHNodeRightChild(data.nodes[nodeIndex]);

        float leftCos = kLightBVHInvalidCosConeAngle;
        float3 leftDir = computeLightingConesInternal(leftIndex, data, leftCos);
        float rightCos = kLightBVHInvalidCosConeAngle;
        float3 rightDir = computeLightingConesInternal(rightIndex, data, rightCos);

        float3 coneDirection = coneUnionOld(leftDir, leftCos, rightDir, rightCos, cosConeAngle);
        data.nodes[nodeIndex].cosConeAngle = cosConeAngle;
        data.nodes[nodeIndex].coneDirection = coneDirection;
        return coneDirection;
    }

    cosConeAngle = data.nodes[nodeIndex].cosConeAngle;
    return data.nodes[nodeIndex].coneDirection;
}

SplitResult computeSplitWithEqual(BuildingData const&, Range const& lightRange, AABB const& nodeBounds,
                                  LightBVHOptions const&)
{
    float3 dimensions = nodeBounds.extent();
    uint32_t dimension = dimensions.z >= dimensions.x && dimensions.z >= dimensions.y
                             ? 2u
                             : (dimensions.y >= dimensions.x ? 1u : 0u);
    SplitResult result;
    result.axis = dimension;
    result.lightIndex = lightRange.middle();
    return result;
}

SplitResult computeSplitWithBinnedSAH(BuildingData const& data, Range const& lightRange, AABB const& nodeBounds,
                                      LightBVHOptions const& parameters)
{
    struct Bin
    {
        AABB bounds;
        uint32_t lightCount = 0;

        Bin& operator|=(LightSortData const& light)
        {
            bounds |= light.bounds;
            ++lightCount;
            return *this;
        }

        Bin& operator|=(Bin const& rhs)
        {
            bounds |= rhs.bounds;
            lightCount += rhs.lightCount;
            return *this;
        }
    };

    std::pair<float, SplitResult> overallBest = {std::numeric_limits<float>::infinity(), {}};
    Vector<Bin> bins(parameters.binCount, data.alloc);
    Vector<float> costs(parameters.binCount - 1u, data.alloc);

    auto binAlongDimension = [&](uint32_t dimension)
    {
        auto getBinId = [&](LightSortData const& ld)
        {
            float bmin = nodeBounds.minPoint[dimension];
            float bmax = nodeBounds.maxPoint[dimension];
            float scale = static_cast<float>(parameters.binCount) / (bmax - bmin);
            float p = ld.bounds.center()[dimension];
            return std::min(static_cast<uint32_t>((p - bmin) * scale), parameters.binCount - 1u);
        };

        for (Bin& bin : bins)
            bin = Bin{};
        for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
            bins[getBinId(data.lightsData[i])] |= data.lightsData[i];

        Bin total{};
        for (size_t i = 0; i < costs.size(); ++i)
        {
            total |= bins[i];
            costs[i] = evalSAH(total.bounds, total.lightCount, parameters);
        }
        total = Bin{};
        for (size_t i = costs.size(); i > 0; --i)
        {
            total |= bins[i];
            costs[i - 1] += evalSAH(total.bounds, total.lightCount, parameters);
        }

        std::pair<float, SplitResult> axisBest = {std::numeric_limits<float>::infinity(),
                                                  SplitResult{dimension, 0}};
        for (uint32_t i = 0, lightIdx = lightRange.begin; i < costs.size(); ++i)
        {
            lightIdx += bins[i].lightCount;
            if (costs[i] < axisBest.first)
                axisBest = {costs[i], SplitResult{dimension, lightIdx}};
        }
        if (axisBest.second.lightIndex == lightRange.begin || axisBest.second.lightIndex == lightRange.end)
            return;
        if (axisBest.first < overallBest.first)
            overallBest = axisBest;
    };

    if (parameters.splitAlongLargest)
    {
        float3 dimensions = nodeBounds.extent();
        uint32_t largest = dimensions.z >= dimensions.x && dimensions.z >= dimensions.y
                               ? 2u
                               : (dimensions.y >= dimensions.x ? 1u : 0u);
        binAlongDimension(largest);
    }
    else
    {
        for (uint32_t dimension = 0; dimension < 3; ++dimension)
            binAlongDimension(dimension);
    }

    if (!overallBest.second.isValid())
    {
        if (lightRange.length() <= parameters.maxLightsPerLeaf)
            return {};
        return computeSplitWithEqual(data, lightRange, nodeBounds, parameters);
    }

    if (parameters.useLeafCreationCost && lightRange.length() <= parameters.maxLightsPerLeaf)
    {
        float leafCost = evalSAH(nodeBounds, lightRange.length(), parameters);
        if (leafCost <= overallBest.first)
            return {};
    }
    return overallBest.second;
}

SplitResult computeSplitWithBinnedSAOH(BuildingData const& data, Range const& lightRange, AABB const& nodeBounds,
                                       LightBVHOptions const& parameters)
{
    struct Bin
    {
        AABB bounds;
        uint32_t lightCount = 0;
        float flux = 0.0f;
        float3 coneDirection{};
        float cosConeAngle = 1.0f;

        Bin& operator|=(LightSortData const& light)
        {
            bounds |= light.bounds;
            ++lightCount;
            flux += light.flux;
            coneDirection += light.coneDirection;
            return *this;
        }

        Bin& operator|=(Bin const& rhs)
        {
            bounds |= rhs.bounds;
            lightCount += rhs.lightCount;
            flux += rhs.flux;
            coneDirection += rhs.coneDirection;
            return *this;
        }
    };

    float3 dimensions = nodeBounds.extent();
    uint32_t largestDimension = dimensions.z >= dimensions.x && dimensions.z >= dimensions.y
                                    ? 2u
                                    : (dimensions.y >= dimensions.x ? 1u : 0u);

    std::pair<float, SplitResult> overallBest = {std::numeric_limits<float>::infinity(), {}};
    Vector<Bin> bins(parameters.binCount, data.alloc);
    Vector<float> costs(parameters.binCount - 1u, data.alloc);

    auto binAlongDimension = [&](uint32_t dimension)
    {
        auto getBinId = [&](LightSortData const& ld)
        {
            float bmin = nodeBounds.minPoint[dimension];
            float bmax = nodeBounds.maxPoint[dimension];
            float w = bmax - bmin;
            float scale = w > FLT_MIN ? static_cast<float>(parameters.binCount) / w : 0.0f;
            float p = ld.bounds.center()[dimension];
            return std::min(static_cast<uint32_t>((p - bmin) * scale), parameters.binCount - 1u);
        };

        for (Bin& bin : bins)
            bin = Bin{};
        for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
            bins[getBinId(data.lightsData[i])] |= data.lightsData[i];

        for (Bin& bin : bins)
        {
            bin.cosConeAngle = length(bin.coneDirection) < FLT_MIN ? kLightBVHInvalidCosConeAngle : 1.0f;
            if (bin.cosConeAngle != kLightBVHInvalidCosConeAngle)
                bin.coneDirection = normalize(bin.coneDirection);
        }
        for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
        {
            LightSortData const& ld = data.lightsData[i];
            Bin& bin = bins[getBinId(ld)];
            bin.cosConeAngle = computeCosConeAngle(bin.coneDirection, bin.cosConeAngle, ld.coneDirection,
                                                   ld.cosConeAngle);
        }

        Bin total{};
        for (size_t i = 0; i < costs.size(); ++i)
        {
            total |= bins[i];
            float cosTheta = kLightBVHInvalidCosConeAngle;
            if (length(total.coneDirection) >= FLT_MIN)
            {
                cosTheta = 1.0f;
                float3 coneDir = normalize(total.coneDirection);
                for (size_t j = 0; j <= i; ++j)
                    cosTheta = computeCosConeAngle(coneDir, cosTheta, bins[j].coneDirection, bins[j].cosConeAngle);
            }
            costs[i] = evalSAOH(total.bounds, total.flux, cosTheta, parameters);
        }

        total = Bin{};
        for (size_t i = costs.size(); i > 0; --i)
        {
            total |= bins[i];
            float cosTheta = kLightBVHInvalidCosConeAngle;
            if (length(total.coneDirection) >= FLT_MIN)
            {
                cosTheta = 1.0f;
                float3 coneDir = normalize(total.coneDirection);
                for (size_t j = i; j <= costs.size(); ++j)
                    cosTheta = computeCosConeAngle(coneDir, cosTheta, bins[j].coneDirection, bins[j].cosConeAngle);
            }
            costs[i - 1] += evalSAOH(total.bounds, total.flux, cosTheta, parameters);
        }

        std::pair<float, SplitResult> axisBest = {std::numeric_limits<float>::infinity(),
                                                  SplitResult{dimension, 0}};
        for (uint32_t i = 0, lightIdx = lightRange.begin; i < costs.size(); ++i)
        {
            lightIdx += bins[i].lightCount;
            if (costs[i] < axisBest.first)
                axisBest = {costs[i], SplitResult{dimension, lightIdx}};
        }
        axisBest.first *= dimensions[largestDimension] / std::max(dimensions[dimension], FLT_MIN);
        if (axisBest.second.lightIndex == lightRange.begin || axisBest.second.lightIndex == lightRange.end)
            return;
        if (axisBest.first < overallBest.first)
            overallBest = axisBest;
    };

    if (parameters.splitAlongLargest)
        binAlongDimension(largestDimension);
    else
    {
        for (uint32_t dimension = 0; dimension < 3; ++dimension)
            binAlongDimension(dimension);
    }

    if (!overallBest.second.isValid())
    {
        if (lightRange.length() <= parameters.maxLightsPerLeaf)
            return {};
        return computeSplitWithEqual(data, lightRange, nodeBounds, parameters);
    }

    if (parameters.useLeafCreationCost && lightRange.length() <= parameters.maxLightsPerLeaf)
    {
        float cosTheta = kLightBVHInvalidCosConeAngle;
        computeLightingCone(lightRange, data, cosTheta);
        float leafCost = evalSAOH(nodeBounds, data.currentNodeFlux, cosTheta, parameters);
        if (leafCost <= overallBest.first)
            return {};
    }
    return overallBest.second;
}

using SplitFn = SplitResult (*)(BuildingData const&, Range const&, AABB const&, LightBVHOptions const&);

SplitFn getSplitFunction(LightBVHSplitHeuristic heuristic)
{
    switch (heuristic)
    {
    case LightBVHSplitHeuristic::Equal:
        return computeSplitWithEqual;
    case LightBVHSplitHeuristic::BinnedSAH:
        return computeSplitWithBinnedSAH;
    case LightBVHSplitHeuristic::BinnedSAOH:
    default:
        return computeSplitWithBinnedSAOH;
    }
}

uint32_t buildInternal(LightBVHOptions const& options, SplitFn splitHeuristic, uint64_t bitmask, uint32_t depth,
                       Range const& lightRange, BuildingData& data)
{
    CHECK(lightRange.begin < lightRange.end);

    float nodeFlux = 0.0f;
    AABB nodeBounds;
    for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
    {
        nodeBounds |= data.lightsData[i].bounds;
        nodeFlux += data.lightsData[i].flux;
    }
    CHECK(nodeBounds.valid());
    data.currentNodeFlux = nodeFlux;

    bool trySplitting = lightRange.length() > (options.createLeavesASAP ? options.maxLightsPerLeaf : 1u);
    SplitResult splitResult = trySplitting ? splitHeuristic(data, lightRange, nodeBounds, options) : SplitResult{};

    if (splitResult.isValid())
    {
        CHECK(lightRange.begin < splitResult.lightIndex && splitResult.lightIndex < lightRange.end);
        auto comp = [dim = splitResult.axis](LightSortData const& a, LightSortData const& b)
        { return a.bounds.center()[dim] < b.bounds.center()[dim]; };
        std::nth_element(data.lightsData.begin() + lightRange.begin, data.lightsData.begin() + splitResult.lightIndex,
                         data.lightsData.begin() + lightRange.end, comp);

        uint32_t nodeIndex = static_cast<uint32_t>(data.nodes.size());
        data.nodes.push_back({});
        GSLightBVHNode& node = data.nodes.back();
        GSLightBVHNodeSetAABB(node, nodeBounds.minPoint, nodeBounds.maxPoint);
        node.flux = nodeFlux;
        node.coneDirection = float3(0.0f);
        node.cosConeAngle = kLightBVHInvalidCosConeAngle;

        CHECK_MSG(depth < kLightBVHMaxDepth, "Light BVH depth {} exceeds max {}", depth + 1u, kLightBVHMaxDepth);

        uint32_t leftIndex =
            buildInternal(options, splitHeuristic, bitmask | (0ull << depth), depth + 1u,
                          Range{lightRange.begin, splitResult.lightIndex}, data);
        uint32_t rightIndex =
            buildInternal(options, splitHeuristic, bitmask | (1ull << depth), depth + 1u,
                          Range{splitResult.lightIndex, lightRange.end}, data);
        CHECK(leftIndex == nodeIndex + 1u);
        GSLightBVHNodeSetInternal(data.nodes[nodeIndex], rightIndex);
        return nodeIndex;
    }

    CHECK(lightRange.length() <= options.maxLightsPerLeaf);
    uint32_t nodeIndex = static_cast<uint32_t>(data.nodes.size());
    data.nodes.push_back({});
    GSLightBVHNode& node = data.nodes.back();
    GSLightBVHNodeSetAABB(node, nodeBounds.minPoint, nodeBounds.maxPoint);
    node.flux = nodeFlux;
    float cosTheta = kLightBVHInvalidCosConeAngle;
    node.coneDirection = computeLightingCone(lightRange, data, cosTheta);
    node.cosConeAngle = cosTheta;

    uint32_t lightCount = lightRange.length();
    uint32_t lightOffset = static_cast<uint32_t>(data.lightIndices.size());
    CHECK(lightCount < kLightBVHMaxLightsPerLeaf);
    CHECK(lightOffset < kLightBVHMaxLightOffset);
    GSLightBVHNodeSetLeaf(node, lightCount, lightOffset);

    for (uint32_t i = lightRange.begin; i < lightRange.end; ++i)
    {
        uint32_t globalIndex = data.lightsData[i].lightIndex;
        data.lightIndices.push_back(globalIndex);
        data.lightBitmasks[globalIndex] = bitmask;
    }
    return nodeIndex;
}

void finalizeNodeIndices(LightBVHBuild& bvh)
{
    if (!bvh.valid || bvh.nodes.empty())
        return;

    CHECK(bvh.allocator);
    bvh.stats = {};
    bvh.stats.byteSize = static_cast<uint32_t>(bvh.nodes.size() * sizeof(GSLightBVHNode));
    bvh.stats.finiteLightCount = static_cast<uint32_t>(bvh.lightIndices.size());
    bvh.stats.globalLightCount = static_cast<uint32_t>(bvh.globalLightIndices.size());
    bvh.stats.minDepth = std::numeric_limits<uint32_t>::max();

    Vector<uint32_t> nodeCountPerLevel(bvh.allocator);
    std::stack<std::pair<uint32_t, uint32_t>> stack;
    stack.push({0u, 0u});
    while (!stack.empty())
    {
        auto [nodeIndex, depth] = stack.top();
        stack.pop();
        if (nodeCountPerLevel.size() <= depth)
            nodeCountPerLevel.resize(depth + 1u, 0u);
        ++nodeCountPerLevel[depth];

        if (GSLightBVHNodeIsLeaf(bvh.nodes[nodeIndex]))
        {
            ++bvh.stats.leafNodeCount;
            bvh.stats.treeHeight = std::max(bvh.stats.treeHeight, depth);
            bvh.stats.minDepth = std::min(bvh.stats.minDepth, depth);
        }
        else
        {
            ++bvh.stats.internalNodeCount;
            stack.push({nodeIndex + 1u, depth + 1u});
            stack.push({GSLightBVHNodeRightChild(bvh.nodes[nodeIndex]), depth + 1u});
        }
    }
    (void)nodeCountPerLevel;
    if (bvh.stats.leafNodeCount == 0)
        bvh.stats.minDepth = 0;

    bvh.refitLevels.assign(bvh.stats.treeHeight + 1u, {});
    bvh.refitLevels.back().count = bvh.stats.leafNodeCount;
    stack.push({0u, 0u});
    while (!stack.empty())
    {
        auto [nodeIndex, depth] = stack.top();
        stack.pop();
        if (!GSLightBVHNodeIsLeaf(bvh.nodes[nodeIndex]))
        {
            ++bvh.refitLevels[depth].count;
            stack.push({nodeIndex + 1u, depth + 1u});
            stack.push({GSLightBVHNodeRightChild(bvh.nodes[nodeIndex]), depth + 1u});
        }
    }

    Vector<uint32_t> perDepthOffset(bvh.refitLevels.size(), 0u, bvh.allocator);
    for (size_t i = 1; i < bvh.refitLevels.size(); ++i)
    {
        uint32_t current = bvh.refitLevels[i - 1].offset + bvh.refitLevels[i - 1].count;
        perDepthOffset[i] = bvh.refitLevels[i].offset = current;
    }

    bvh.nodeIndices.assign(bvh.stats.internalNodeCount + bvh.stats.leafNodeCount, 0u);
    stack.push({0u, 0u});
    while (!stack.empty())
    {
        auto [nodeIndex, depth] = stack.top();
        stack.pop();
        if (GSLightBVHNodeIsLeaf(bvh.nodes[nodeIndex]))
            bvh.nodeIndices[perDepthOffset.back()++] = nodeIndex;
        else
        {
            bvh.nodeIndices[perDepthOffset[depth]++] = nodeIndex;
            stack.push({nodeIndex + 1u, depth + 1u});
            stack.push({GSLightBVHNodeRightChild(bvh.nodes[nodeIndex]), depth + 1u});
        }
    }
}
} // namespace

void ComputeAnalyticalLightBounds(GSLight const& light, float3& aabbMin, float3& aabbMax, float3& center,
                                  float3& coneDirection, float& cosConeAngle)
{
    uint32_t type = GSLightTypeCPU(light);
    center = light.position;
    coneDirection = float3(0.0f);
    cosConeAngle = kLightBVHInvalidCosConeAngle;

    if (type == kGSLightTypePoint)
    {
        float radius = std::max(light.params.x, 1e-3f);
        aabbMin = light.position - float3(radius);
        aabbMax = light.position + float3(radius);
        return;
    }
    if (type == kGSLightTypeSpot)
    {
        float radius = std::max(light.params.x, 1e-3f);
        aabbMin = light.position - float3(radius);
        aabbMax = light.position + float3(radius);
        coneDirection = normalize(light.direction);
        cosConeAngle = std::clamp(light.params.z, -1.0f, 1.0f);
        return;
    }
    if (type == kGSLightTypeDisk)
    {
        float3 u = light.dpdu * light.params.x;
        float3 v = light.dpdv * light.params.y;
        AABB bounds;
        bounds |= light.position + u + v;
        bounds |= light.position + u - v;
        bounds |= light.position - u + v;
        bounds |= light.position - u - v;
        aabbMin = bounds.minPoint;
        aabbMax = bounds.maxPoint;
        center = light.position;
        if ((light.flags & kGSLightFlagTwoSided) == 0u)
        {
            coneDirection = normalize(light.direction);
            cosConeAngle = 1.0f;
        }
        return;
    }
    if (type == kGSLightTypeRect)
    {
        AABB bounds;
        bounds |= light.position + light.dpdu + light.dpdv;
        bounds |= light.position + light.dpdu - light.dpdv;
        bounds |= light.position - light.dpdu + light.dpdv;
        bounds |= light.position - light.dpdu - light.dpdv;
        aabbMin = bounds.minPoint;
        aabbMax = bounds.maxPoint;
        center = light.position;
        if ((light.flags & kGSLightFlagTwoSided) == 0u)
        {
            coneDirection = normalize(light.direction);
            cosConeAngle = 1.0f;
        }
        return;
    }

    aabbMin = float3(0.0f);
    aabbMax = float3(0.0f);
    center = float3(0.0f);
}

LightBVHBuild BuildLightBVH(Span<GSLight const> lights, LightBVHOptions const& options, Allocator* alloc)
{
    CHECK(alloc);
    LightBVHBuild bvh(alloc);
    bvh.lightBitmasks.assign(lights.size(), std::numeric_limits<uint64_t>::max());

    CHECK_MSG(options.maxLightsPerLeaf > 0u && options.maxLightsPerLeaf < kLightBVHMaxLightsPerLeaf,
              "maxLightsPerLeaf {} out of range", options.maxLightsPerLeaf);
    CHECK_MSG(options.binCount > 1u, "binCount must be > 1");

    BuildingData data(bvh.nodes, alloc);
    data.lightsData.reserve(lights.size());
    data.lightIndices.reserve(lights.size());
    data.lightBitmasks = bvh.lightBitmasks;

    for (uint32_t i = 0; i < lights.size(); ++i)
    {
        GSLight const& light = lights[i];
        uint32_t type = GSLightTypeCPU(light);
        if (IsGlobalLightType(type))
        {
            bvh.globalLightIndices.push_back(i);
            continue;
        }
        if (!IsFiniteLightType(type))
            continue;

        float flux = std::max(0.0f, light.importance);
        if (options.usePreintegration && flux <= 0.0f)
            continue;

        LightSortData sort{};
        ComputeAnalyticalLightBounds(light, sort.bounds.minPoint, sort.bounds.maxPoint, sort.center,
                                     sort.coneDirection, sort.cosConeAngle);
        sort.flux = flux > 0.0f ? flux : 1.0f;
        sort.lightIndex = i;
        data.lightsData.push_back(sort);
    }

    if (data.lightsData.empty())
    {
        bvh.stats.globalLightCount = static_cast<uint32_t>(bvh.globalLightIndices.size());
        bvh.lightBitmasks = std::move(data.lightBitmasks);
        return bvh;
    }

    CHECK_MSG(data.lightsData.size() <= kLightBVHMaxLightOffset + kLightBVHMaxLightsPerLeaf,
              "Finite light count exceeds Light BVH encoding limits");

    data.nodes.reserve(2u * data.lightsData.size());
    SplitFn splitFn = getSplitFunction(options.splitHeuristic);
    buildInternal(options, splitFn, 0ull, 0u, Range{0u, static_cast<uint32_t>(data.lightsData.size())}, data);

    float unusedCos = kLightBVHInvalidCosConeAngle;
    computeLightingConesInternal(0u, data, unusedCos);

    bvh.nodes = std::move(data.nodes);
    bvh.lightIndices = std::move(data.lightIndices);
    bvh.lightBitmasks = std::move(data.lightBitmasks);
    bvh.valid = !bvh.nodes.empty();
    finalizeNodeIndices(bvh);
    return bvh;
}

bool ValidateLightBVH(LightBVHBuild const& bvh, Span<GSLight const> lights, String* outError)
{
    auto fail = [&](char const* msg) -> bool
    {
        if (outError)
            *outError = msg;
        return false;
    };

    if (bvh.lightBitmasks.size() != lights.size())
        return fail("lightBitmasks size mismatch");

    uint32_t expectedGlobal = 0;
    uint32_t expectedFinite = 0;
    for (uint32_t i = 0; i < lights.size(); ++i)
    {
        uint32_t type = GSLightTypeCPU(lights[i]);
        if (IsGlobalLightType(type))
            ++expectedGlobal;
        else if (IsFiniteLightType(type) && lights[i].importance > 0.0f)
            ++expectedFinite;
    }

    if (bvh.globalLightIndices.size() != expectedGlobal)
        return fail("global light count mismatch");
    for (uint32_t idx : bvh.globalLightIndices)
    {
        if (idx >= lights.size() || !IsGlobalLightType(GSLightTypeCPU(lights[idx])))
            return fail("invalid global light index");
    }

    if (!bvh.valid)
    {
        if (expectedFinite != 0)
            return fail("BVH invalid despite finite lights");
        return true;
    }

    if (bvh.nodes.empty())
        return fail("valid BVH has no nodes");
    if (bvh.lightIndices.size() != expectedFinite)
        return fail("finite light membership mismatch");

    std::vector<uint8_t> covered(lights.size(), 0u);
    std::stack<std::pair<uint32_t, uint32_t>> stack;
    stack.push({0u, 0u});
    while (!stack.empty())
    {
        auto [nodeIndex, depth] = stack.top();
        stack.pop();
        if (nodeIndex >= bvh.nodes.size() || depth > kLightBVHMaxDepth)
            return fail("node index/depth out of range");

        GSLightBVHNode const& node = bvh.nodes[nodeIndex];
        float3 aabbMin, aabbMax;
        GSLightBVHNodeGetAABB(node, aabbMin, aabbMax);
        if (!all(lessThanEqual(aabbMin, aabbMax)))
            return fail("invalid node AABB");
        if (!(node.flux >= 0.0f))
            return fail("negative node flux");

        if (GSLightBVHNodeIsLeaf(node))
        {
            uint32_t count = GSLightBVHNodeLightCount(node);
            uint32_t offset = GSLightBVHNodeLightOffset(node);
            if (count == 0 || offset + count > bvh.lightIndices.size())
                return fail("leaf light range out of bounds");

            AABB leafBounds;
            float leafFlux = 0.0f;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t lightIndex = bvh.lightIndices[offset + i];
                if (lightIndex >= lights.size() || !IsFiniteLightType(GSLightTypeCPU(lights[lightIndex])))
                    return fail("leaf contains non-finite light");
                if (covered[lightIndex])
                    return fail("duplicate finite light in leaves");
                covered[lightIndex] = 1u;

                float3 cMin, cMax, center, coneDir;
                float cosCone = kLightBVHInvalidCosConeAngle;
                ComputeAnalyticalLightBounds(lights[lightIndex], cMin, cMax, center, coneDir, cosCone);
                leafBounds |= AABB{cMin, cMax};
                leafFlux += std::max(0.0f, lights[lightIndex].importance);

                uint64_t mask = bvh.lightBitmasks[lightIndex];
                uint32_t replay = 0u;
                uint64_t bits = mask;
                for (uint32_t d = 0; d < depth; ++d)
                {
                    if (GSLightBVHNodeIsLeaf(bvh.nodes[replay]))
                        return fail("path mask reached leaf early");
                    bool goRight = (bits & 1ull) != 0ull;
                    replay = goRight ? GSLightBVHNodeRightChild(bvh.nodes[replay]) : replay + 1u;
                    bits >>= 1;
                }
                if (replay != nodeIndex)
                    return fail("path mask does not replay to leaf");
            }
            if (any(lessThan(leafBounds.minPoint, aabbMin - float3(1e-3f))) ||
                any(greaterThan(leafBounds.maxPoint, aabbMax + float3(1e-3f))))
                return fail("leaf AABB does not cover children");
            if (leafFlux > node.flux + 1e-3f)
                return fail("leaf flux less than child sum");
        }
        else
        {
            uint32_t left = nodeIndex + 1u;
            uint32_t right = GSLightBVHNodeRightChild(node);
            if (left >= bvh.nodes.size() || right >= bvh.nodes.size() || right <= left)
                return fail("invalid internal child indices");
            stack.push({left, depth + 1u});
            stack.push({right, depth + 1u});
        }
    }

    for (uint32_t i = 0; i < lights.size(); ++i)
    {
        if (IsFiniteLightType(GSLightTypeCPU(lights[i])) && lights[i].importance > 0.0f && !covered[i])
            return fail("finite light missing from BVH");
    }
    return true;
}

namespace
{
GSLight MakeTestLight(uint32_t type, float3 position, float importance, float3 direction = float3(0, 0, -1))
{
    GSLight light{};
    light.flags = type | kGSLightFlagUseShadow;
    light.color = float3(1.0f);
    light.power = 1.0f;
    light.position = position;
    light.direction = normalize(direction);
    light.importance = importance;
    if (type == kGSLightTypePoint || type == kGSLightTypeSpot)
        light.params.x = 0.1f;
    if (type == kGSLightTypeSpot)
        light.params = float4(0.1f, 0.9f, 0.7f, 0.0f);
    if (type == kGSLightTypeDisk)
    {
        light.params = float4(0.5f, 0.5f, 0.0f, 0.0f);
        light.dpdu = float3(1, 0, 0);
        light.dpdv = float3(0, 1, 0);
    }
    if (type == kGSLightTypeRect)
    {
        light.dpdu = float3(0.5f, 0, 0);
        light.dpdv = float3(0, 0.5f, 0);
    }
    return light;
}

bool RunCase(Allocator* alloc, Span<GSLight const> lights, LightBVHOptions const& options, char const* name,
             String* outError)
{
    LightBVHBuild bvh = BuildLightBVH(lights, options, alloc);
    String error;
    if (!ValidateLightBVH(bvh, lights, &error))
    {
        if (outError)
            *outError = String(name) + ": " + error;
        return false;
    }
    return true;
}
} // namespace

bool LightBVHRunBuilderSelfTests(Allocator* alloc, String* outError)
{
    CHECK(alloc);
    LightBVHOptions options{};

    {
        Vector<GSLight> lights(alloc);
        lights.push_back(MakeTestLight(kGSLightTypeEnvironment, float3(0), 10.0f));
        if (!RunCase(alloc, lights, options, "env-only", outError))
            return false;
    }
    {
        Vector<GSLight> lights(alloc);
        lights.push_back(MakeTestLight(kGSLightTypeEnvironment, float3(0), 10.0f));
        lights.push_back(MakeTestLight(kGSLightTypePoint, float3(1, 2, 3), 4.0f));
        if (!RunCase(alloc, lights, options, "one-point", outError))
            return false;
    }
    {
        Vector<GSLight> lights(alloc);
        lights.push_back(MakeTestLight(kGSLightTypeEnvironment, float3(0), 10.0f));
        lights.push_back(MakeTestLight(kGSLightTypeDirectional, float3(0), 8.0f, float3(0, -1, 0)));
        lights.push_back(MakeTestLight(kGSLightTypePoint, float3(1, 0, 0), 2.0f));
        lights.push_back(MakeTestLight(kGSLightTypeSpot, float3(0, 1, 0), 3.0f, float3(0, -1, 0)));
        lights.push_back(MakeTestLight(kGSLightTypeDisk, float3(0, 2, 0), 5.0f, float3(0, -1, 0)));
        lights.push_back(MakeTestLight(kGSLightTypeRect, float3(2, 0, 0), 6.0f, float3(-1, 0, 0)));
        lights.push_back(MakeTestLight(kGSLightTypePoint, float3(5, 5, 5), 0.0f));
        if (!RunCase(alloc, lights, options, "mixed", outError))
            return false;
    }
    {
        LightBVHOptions manyOpts = options;
        manyOpts.maxLightsPerLeaf = 1u;
        manyOpts.splitHeuristic = LightBVHSplitHeuristic::Equal;
        Vector<GSLight> lights(alloc);
        lights.push_back(MakeTestLight(kGSLightTypeEnvironment, float3(0), 1.0f));
        for (uint32_t i = 0; i < 8; ++i)
            lights.push_back(MakeTestLight(kGSLightTypePoint, float3(float(i), 0, float(i * 2)), 1.0f + float(i)));
        if (!RunCase(alloc, lights, manyOpts, "many-leaves", outError))
            return false;
        LightBVHBuild bvh = BuildLightBVH(lights, manyOpts, alloc);
        if (!bvh.valid || bvh.stats.leafNodeCount < 2u)
        {
            if (outError)
                *outError = "many-leaves: expected multiple leaf nodes";
            return false;
        }
    }
    return true;
}
